#include <gtk/gtk.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

// Structure to hold application data
typedef struct {
    GtkWidget *main_window;
    GtkWidget *habits_vbox;
    GList *habits;        // Dynamic list of habit names (char *)
    GList *habit_widgets; // Dynamic list of habit widgets (GtkWidget *)
    sqlite3 *db;
    GtkWidget *habits_box; // Container for habits
    GtkWidget *pending_tasks_box; // Container for pending tasks
    GtkWidget *completed_tasks_box; // Container for completed tasks
    GtkWidget *timetable_grid; // Reference to the timetable grid
} AppData;

// Callback for when a habit checkbox is toggled (used in edit window)
static void on_habit_toggled(GtkCheckButton *check_button, AppData *app_data) {
    const char *habit_name = gtk_check_button_get_label(check_button);
    gboolean completed = gtk_check_button_get_active(check_button);

    GDateTime *date = g_date_time_new_now_local();
    if (date == NULL) return;

    gint year = g_date_time_get_year(date);
    gint month = g_date_time_get_month(date);
    gint day = g_date_time_get_day_of_month(date);

    g_date_time_unref(date);

    char query[256];
    snprintf(query, sizeof(query),
             "INSERT OR REPLACE INTO habit_tracking (date, habit_name, completed) VALUES ('%04d-%02d-%02d', '%s', %d);",
             year, month, day, habit_name, completed);
    sqlite3_exec(app_data->db, query, NULL, NULL, NULL);
}

// Function to count days a habit was completed
static int get_days_completed(sqlite3 *db, const char *habit_name) {
    char query[256];
    snprintf(query, sizeof(query),
             "SELECT COUNT(DISTINCT date) FROM habit_tracking WHERE habit_name = '%s' AND completed = 1;",
             habit_name);

    sqlite3_stmt *stmt;
    int days = 0;
    if (sqlite3_prepare_v2(db, query, -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            days = sqlite3_column_int(stmt, 0);
        }
    }
    sqlite3_finalize(stmt);
    return days;
}

// Callback to draw a circular habit logo with days completed
static void draw_habit_logo(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data) {
    const char *habit_name = (const char *)data;
    AppData *app_data = g_object_get_data(G_OBJECT(area), "app_data");
    int days = get_days_completed(app_data->db, habit_name);

    int radius = MIN(width, height) / 2 - 5;

    // Draw circle
    cairo_arc(cr, width / 2, height / 2, radius, 0, 2 * G_PI);
    cairo_set_source_rgb(cr, 0.2, 0.6, 0.2); // Green color for the circle
    cairo_fill(cr);

    // Draw days completed in the center
    char days_str[16];
    snprintf(days_str, sizeof(days_str), "%d", days);
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0); // White color for text
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 20);
    cairo_text_extents_t extents;
    cairo_text_extents(cr, days_str, &extents);
    cairo_move_to(cr, width / 2 - extents.width / 2, height / 2 + extents.height / 2);
    cairo_show_text(cr, days_str);
}

// Callback to remove a habit
static void on_remove_habit(GtkButton *button, AppData *app_data) {
    const char *habit_name = (const char *)g_object_get_data(G_OBJECT(button), "habit_name");
    GtkWidget *row = (GtkWidget *)g_object_get_data(G_OBJECT(button), "row");

    if (!habit_name || !row) {
        g_printerr("Error: Invalid habit_name or row in on_remove_habit\n");
        return;
    }

    g_print("Removing habit: %s\n", habit_name);

    // Remove from habits list
    GList *link = g_list_find_custom(app_data->habits, habit_name, (GCompareFunc)strcmp);
    if (link) {
        g_free(link->data); // Free the habit name
        app_data->habits = g_list_delete_link(app_data->habits, link);
        g_print("Habit removed from habits list\n");
    } else {
        g_printerr("Error: Habit %s not found in habits list\n", habit_name);
    }

    // Remove from UI in edit window
    gtk_grid_remove(GTK_GRID(gtk_widget_get_parent(row)), row);
    gtk_widget_unparent(row); // Unparent the row to allow it to be destroyed
    g_print("Habit row removed from edit window\n");

    // Remove from main page via habit_widgets list
    GList *iter = app_data->habit_widgets;
    gboolean found = FALSE;
    while (iter) {
        GtkWidget *widget = (GtkWidget *)iter->data;
        const char *name = (const char *)g_object_get_data(G_OBJECT(widget), "habit_name");
        if (name && strcmp(name, habit_name) == 0) {
            g_print("Found habit widget in habit_widgets list: %s\n", name);
            // Remove from UI
            gtk_box_remove(GTK_BOX(app_data->habits_box), widget);
            // Remove from list
            app_data->habit_widgets = g_list_remove(app_data->habit_widgets, widget);
            // Free the habit name stored in the widget
            g_free((char *)name);
            // Unparent the widget to allow it to be destroyed
            gtk_widget_unparent(widget);
            found = TRUE;
            break;
        }
        iter = iter->next;
    }

    // Fallback: If not found in habit_widgets, search habits_box directly
    if (!found) {
        g_printerr("Error: Habit widget %s not found in habit_widgets list, searching habits_box directly\n", habit_name);
        GtkWidget *child = gtk_widget_get_first_child(app_data->habits_box);
        while (child) {
            // Check the habit_box itself
            const char *name = (const char *)g_object_get_data(G_OBJECT(child), "habit_name");
            if (name && strcmp(name, habit_name) == 0) {
                g_print("Found habit widget in habits_box: %s\n", name);
                gtk_box_remove(GTK_BOX(app_data->habits_box), child);
                g_free((char *)name);
                gtk_widget_unparent(child);
                found = TRUE;
                break;
            }

            // Check the drawing_area child (for backward compatibility)
            GtkWidget *drawing_area = gtk_widget_get_first_child(child);
            if (drawing_area) {
                name = (const char *)g_object_get_data(G_OBJECT(drawing_area), "habit_name");
                if (name && strcmp(name, habit_name) == 0) {
                    g_print("Found habit widget in habits_box (via drawing_area): %s\n", name);
                    gtk_box_remove(GTK_BOX(app_data->habits_box), child);
                    g_free((char *)name);
                    gtk_widget_unparent( child);
                    found = TRUE;
                    break;
                }
            }

            child = gtk_widget_get_next_sibling(child);
        }
    }

    if (found) {
        g_print("Habit widget removed from main page\n");
        // Force UI update
        gtk_widget_queue_draw(app_data->habits_box);
        g_print("Forced UI update on habits_box\n");
    } else {
        g_printerr("Error: Habit widget %s not found in habits_box either\n", habit_name);
    }

    // Remove from timetable UI
    const char *days[] = {"", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    const char *times[] = {
        "00:00-02:00", "02:00-04:00", "04:00-06:00", "06:00-08:00",
        "08:00-10:00", "10:00-12:00", "12:00-14:00", "14:00-16:00",
        "16:00-18:00", "18:00-20:00", "20:00-22:00", "22:00-24:00"
    };
    // Fetch the time slot for the habit
    char query[256];
    snprintf(query, sizeof(query), "SELECT days, time_slot FROM habit_tracking WHERE habit_name = '%s';", habit_name);
    sqlite3_stmt *stmt;
    const char *days_str = "";
    const char *time_slot = "";
    if (sqlite3_prepare_v2(app_data->db, query, -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            days_str = (const char *)sqlite3_column_text(stmt, 0);
            time_slot = (const char *)sqlite3_column_text(stmt, 1);
            if (!time_slot) time_slot = "";
        }
    }
    sqlite3_finalize(stmt);

    if (days_str && time_slot && strlen(time_slot) > 0) {
        int time_idx = 0;
        for (int i = 0; i < 12; i++) {
            if (strcmp(times[i], time_slot) == 0) {
                time_idx = i + 1;
                break;
            }
        }

        // Split the days string (e.g., "Mon,Tue,Wed")
        char *days_copy = g_strdup(days_str);
        char *token = strtok(days_copy, ",");
        while (token) {
            int day_idx = 0;
            for (int i = 1; i <= 7; i++) {
                if (strcmp(days[i], token) == 0) {
                    day_idx = i;
                    break;
                }
            }
            if (day_idx > 0 && time_idx > 0 && app_data->timetable_grid) {
                GtkWidget *task_box = gtk_grid_get_child_at(GTK_GRID(app_data->timetable_grid), day_idx, time_idx);
                if (task_box) {
                    GtkWidget *child = gtk_widget_get_first_child(task_box);
                    while (child) {
                        const char *label_text = gtk_label_get_text(GTK_LABEL(child));
                        if (strcmp(label_text, habit_name) == 0) {
                            gtk_box_remove(GTK_BOX(task_box), child);
                            break;
                        }
                        child = gtk_widget_get_next_sibling(child);
                    }
                }
            }
            token = strtok(NULL, ",");
        }
        g_free(days_copy);
    }

    // Remove from database
    snprintf(query, sizeof(query), "DELETE FROM habit_tracking WHERE habit_name = '%s';", habit_name);
    sqlite3_exec(app_data->db, query, NULL, NULL, NULL);
    g_print("Habit removed from database\n");
}

// Callback to update habit days and time in the database
static void on_day_toggled(GtkToggleButton *toggle_button, gpointer data) {
    AppData *app_data = (AppData *)g_object_get_data(G_OBJECT(toggle_button), "app_data");
    const char *habit_name = (const char *)g_object_get_data(G_OBJECT(toggle_button), "habit_name");
    GtkWidget *hour_dropdown = (GtkWidget *)g_object_get_data(G_OBJECT(toggle_button), "hour_dropdown");

    // Collect selected days
    GString *days = g_string_new("");
    const char *day_names[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    for (int i = 0; i < 7; i++) {
        GtkWidget *day_button = (GtkWidget *)g_object_get_data(G_OBJECT(toggle_button), day_names[i]);
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(day_button))) {
            if (days->len > 0) g_string_append(days, ",");
            g_string_append(days, day_names[i]);
        }
    }

    // Get selected time slot
    const char *times[] = {
        "00:00-02:00", "02:00-04:00", "04:00-06:00", "06:00-08:00",
        "08:00-10:00", "10:00-12:00", "12:00-14:00", "14:00-16:00",
        "16:00-18:00", "18:00-20:00", "20:00-22:00", "22:00-24:00"
    };
    guint selected_time = gtk_drop_down_get_selected(GTK_DROP_DOWN(hour_dropdown));
    const char *time_slot = times[selected_time];

    // Remove existing entries in timetable UI for this habit
    for (int day = 1; day <= 7; day++) {
        for (int time = 1; time <= 12; time++) {
            GtkWidget *task_box = gtk_grid_get_child_at(GTK_GRID(app_data->timetable_grid), day, time);
            if (task_box) {
                GtkWidget *child = gtk_widget_get_first_child(task_box);
                while (child) {
                    const char *label_text = gtk_label_get_text(GTK_LABEL(child));
                    if (strcmp(label_text, habit_name) == 0) {
                        gtk_box_remove(GTK_BOX(task_box), child);
                        break;
                    }
                    child = gtk_widget_get_next_sibling(child);
                }
            }
        }
    }

    // Add habit to timetable UI for selected days and time
    int time_idx = selected_time + 1;
    char *days_copy = g_strdup(days->str);
    char *token = strtok(days_copy, ",");
    while (token) {
        int day_idx = 0;
        for (int i = 1; i <= 7; i++) {
            if (strcmp(day_names[i - 1], token) == 0) {
                day_idx = i;
                break;
            }
        }
        if (day_idx > 0 && app_data->timetable_grid) {
            GtkWidget *task_box = gtk_grid_get_child_at(GTK_GRID(app_data->timetable_grid), day_idx, time_idx);
            if (task_box) {
                GtkWidget *habit_label = gtk_label_new(habit_name);
                gtk_widget_set_halign(habit_label, GTK_ALIGN_START);
                gtk_widget_add_css_class(habit_label, "habit-label");
                gtk_box_append(GTK_BOX(task_box), habit_label);
            }
        }
        token = strtok(NULL, ",");
    }
    g_free(days_copy);

    // Update database
    char query[256];
    snprintf(query, sizeof(query),
             "UPDATE habit_tracking SET days = '%s', time_slot = '%s' WHERE habit_name = '%s';",
             days->str, time_slot, habit_name);
    if (sqlite3_exec(app_data->db, query, NULL, NULL, NULL) != SQLITE_OK) {
        g_printerr("Failed to update days and time for habit %s: %s\n", habit_name, sqlite3_errmsg(app_data->db));
    } else {
        g_print("Updated days and time for habit %s: %s at %s\n", habit_name, days->str, time_slot);
    }

    g_string_free(days, TRUE);
}

// Callback to add a habit in the edit window
static void on_add_habit_in_edit(GtkButton *button, gpointer data) {
    AppData *app_data = (AppData *)g_object_get_data(G_OBJECT(button), "app_data");
    GtkWidget *entry = (GtkWidget *)g_object_get_data(G_OBJECT(button), "entry");
    GtkWidget *habits_grid = (GtkWidget *)g_object_get_data(G_OBJECT(button), "habits_grid");
    GtkWidget *days_box = (GtkWidget *)g_object_get_data(G_OBJECT(button), "days_box");
    GtkWidget *hour_dropdown = (GtkWidget *)g_object_get_data(G_OBJECT(button), "hour_dropdown");

    // Define times array once at the start of the function
    const char *times[] = {
        "00:00-02:00", "02:00-04:00", "04:00-06:00", "06:00-08:00",
        "08:00-10:00", "10:00-12:00", "12:00-14:00", "14:00-16:00",
        "16:00-18:00", "18:00-20:00", "20:00-22:00", "22:00-24:00"
    };

    const char *habit_name = gtk_editable_get_text(GTK_EDITABLE(entry));
    if (habit_name && strlen(habit_name) > 0) {
        g_print("Adding habit: %s\n", habit_name);

        // Collect selected days
        GString *days = g_string_new("");
        const char *day_names[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
        for (int i = 0; i < 7; i++) {
            GtkWidget *day_button = (GtkWidget *)g_object_get_data(G_OBJECT(days_box), day_names[i]);
            if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(day_button))) {
                if (days->len > 0) g_string_append(days, ",");
                g_string_append(days, day_names[i]);
            }
        }

        // Get selected time slot (using the times array defined above)
        guint selected_time = gtk_drop_down_get_selected(GTK_DROP_DOWN(hour_dropdown));
        const char *time_slot = times[selected_time];

        // Add to habits list
        app_data->habits = g_list_append(app_data->habits, g_strdup(habit_name));
        g_print("Habit added to habits list\n");

        // Add to main page
        GtkWidget *habit_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
        GtkWidget *drawing_area = gtk_drawing_area_new();
        gtk_widget_set_size_request(drawing_area, 60, 60);
        gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(drawing_area), draw_habit_logo, g_strdup(habit_name), g_free);
        g_object_set_data(G_OBJECT(drawing_area), "app_data", app_data);
        g_object_set_data(G_OBJECT(drawing_area), "habit_name", g_strdup(habit_name));
        gtk_box_append(GTK_BOX(habit_box), drawing_area);

        // Set habit_name on the habit_box itself
        g_object_set_data(G_OBJECT(habit_box), "habit_name", g_strdup(habit_name));

        GtkWidget *label = gtk_label_new(habit_name);
        gtk_widget_set_halign(label, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(habit_box), label);

        gtk_box_append(GTK_BOX(app_data->habits_box), habit_box);
        app_data->habit_widgets = g_list_append(app_data->habit_widgets, habit_box);
        g_print("Habit widget added to main page and habit_widgets list\n");

        // Add to database with days and time slot
        char query[256];
        snprintf(query, sizeof(query),
                 "INSERT OR IGNORE INTO habit_tracking (date, habit_name, completed, days, time_slot) VALUES ('', '%s', 0, '%s', '%s');",
                 habit_name, days->str, time_slot);
        if (sqlite3_exec(app_data->db, query, NULL, NULL, NULL) != SQLITE_OK) {
            g_printerr("Failed to add habit to database: %s\n", sqlite3_errmsg(app_data->db));
        }

        // Add to timetable UI
        int time_idx = selected_time + 1;
        char *days_copy = g_strdup(days->str);
        char *token = strtok(days_copy, ",");
        while (token) {
            int day_idx = 0;
            for (int i = 1; i <= 7; i++) {
                if (strcmp(day_names[i - 1], token) == 0) {
                    day_idx = i;
                    break;
                }
            }
            if (day_idx > 0 && app_data->timetable_grid) {
                GtkWidget *task_box = gtk_grid_get_child_at(GTK_GRID(app_data->timetable_grid), day_idx, time_idx);
                if (task_box) {
                    GtkWidget *habit_label = gtk_label_new(habit_name);
                    gtk_widget_set_halign(habit_label, GTK_ALIGN_START);
                    gtk_widget_add_css_class(habit_label, "habit-label");
                    gtk_box_append(GTK_BOX(task_box), habit_label);
                }
            }
            token = strtok(NULL, ",");
        }
        g_free(days_copy);

        // Calculate the next row for the grid
        int row = 1; // Start after the header row
        GtkWidget *child = gtk_widget_get_first_child(habits_grid);
        while (child) {
            row++;
            child = gtk_widget_get_next_sibling(child);
        }

        // Add to edit window grid
        GtkWidget *row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
        GtkWidget *row_label = gtk_label_new(habit_name);
        gtk_widget_set_size_request(row_label, 100, -1);
        gtk_box_append(GTK_BOX(row_box), row_label);

        // Add toggle buttons for days
        for (int i = 0; i < 7; i++) {
            GtkWidget *day_button = gtk_toggle_button_new_with_label(day_names[i]);
            gtk_widget_set_size_request(day_button, 50, -1);
            // Check if the day is in the selected days
            if (strstr(days->str, day_names[i])) {
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(day_button), TRUE);
            }
            g_object_set_data(G_OBJECT(day_button), "app_data", app_data);
            g_object_set_data(G_OBJECT(day_button), "habit_name", g_strdup(habit_name));
            // Store references to all day buttons and hour dropdown for this habit
            for (int j = 0; j < 7; j++) {
                g_object_set_data(G_OBJECT(day_button), day_names[j], day_button);
            }
            g_object_set_data(G_OBJECT(day_button), "hour_dropdown", hour_dropdown);
            g_signal_connect(day_button, "toggled", G_CALLBACK(on_day_toggled), NULL);
            gtk_box_append(GTK_BOX(row_box), day_button);
        }

        // Add time slot dropdown (use the times array defined above)
        GtkStringList *times_list = gtk_string_list_new(NULL);
        for (int i = 0; i < 12; i++) {
            gtk_string_list_append(times_list, times[i]);
        }
        GtkWidget *row_hour_dropdown = gtk_drop_down_new(G_LIST_MODEL(times_list), NULL);
        gtk_drop_down_set_selected(GTK_DROP_DOWN(row_hour_dropdown), selected_time);
        gtk_widget_set_size_request(row_hour_dropdown, 120, -1);
        g_object_set_data(G_OBJECT(row_hour_dropdown), "app_data", app_data);
        g_object_set_data(G_OBJECT(row_hour_dropdown), "habit_name", g_strdup(habit_name));
        // Store references for day buttons
        for (int i = 0; i < 7; i++) {
            GtkWidget *day_button = (GtkWidget *)g_object_get_data(G_OBJECT(row_box), day_names[i]);
            if (day_button) {
                g_object_set_data(G_OBJECT(day_button), "hour_dropdown", row_hour_dropdown);
            }
        }
        g_signal_connect(row_hour_dropdown, "notify::selected", G_CALLBACK(on_day_toggled), NULL);
        gtk_box_append(GTK_BOX(row_box), row_hour_dropdown);

        GtkWidget *remove_button = gtk_button_new_with_label("Remove");
        g_object_set_data(G_OBJECT(remove_button), "habit_name", g_strdup(habit_name));
        g_object_set_data(G_OBJECT(remove_button), "row", row_box);
        g_signal_connect(remove_button, "clicked", G_CALLBACK(on_remove_habit), app_data);
        gtk_box_append(GTK_BOX(row_box), remove_button);

        gtk_grid_attach(GTK_GRID(habits_grid), row_box, 0, row, 1, 1);
        g_print("Habit added to edit window\n");

        // Clear entry and reset day toggles and time
        gtk_editable_set_text(GTK_EDITABLE(entry), "");
        for (int i = 0; i < 7; i++) {
            GtkWidget *day_button = (GtkWidget *)g_object_get_data(G_OBJECT(days_box), day_names[i]);
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(day_button), FALSE);
        }
        gtk_drop_down_set_selected(GTK_DROP_DOWN(hour_dropdown), 0);

        g_string_free(days, TRUE);
    }
}
// Callback to open the edit habits window
static void on_edit_habits(GtkButton *button, AppData *app_data) {
    GtkWidget *edit_window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(edit_window), "Edit Habits");
    gtk_window_set_default_size(GTK_WINDOW(edit_window), 600, 400);
    gtk_window_set_transient_for(GTK_WINDOW(edit_window), GTK_WINDOW(app_data->main_window));
    gtk_window_set_modal(GTK_WINDOW(edit_window), TRUE);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(vbox, 10);
    gtk_widget_set_margin_end(vbox, 10);
    gtk_widget_set_margin_top(vbox, 10);
    gtk_widget_set_margin_bottom(vbox, 10);
    gtk_window_set_child(GTK_WINDOW(edit_window), vbox);

    // Grid to display habits with days and time
    GtkWidget *habits_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(habits_grid), 5);
    gtk_grid_set_column_spacing(GTK_GRID(habits_grid), 5);

    // Header row
    const char *labels[] = {"Habit", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun", "Time Slot", "Action"};
    for (int i = 0; i < 10; i++) {
        GtkWidget *label = gtk_label_new(labels[i]);
        gtk_widget_set_halign(label, GTK_ALIGN_START);
        gtk_grid_attach(GTK_GRID(habits_grid), label, i, 0, 1, 1);
    }

    // Populate existing habits
    int row = 1;
    for (GList *iter = app_data->habits; iter; iter = iter->next) {
        const char *habit_name = (const char *)iter->data;

        GtkWidget *row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
        GtkWidget *label = gtk_label_new(habit_name);
        gtk_widget_set_size_request(label, 100, -1);
        gtk_box_append(GTK_BOX(row_box), label);

        // Fetch days and time slot from database
        char query[256];
        snprintf(query, sizeof(query), "SELECT days, time_slot FROM habit_tracking WHERE habit_name = '%s';", habit_name);
        sqlite3_stmt *stmt;
        const char *days_str = "";
        const char *time_slot = "";
        if (sqlite3_prepare_v2(app_data->db, query, -1, &stmt, NULL) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                const char *db_days = (const char *)sqlite3_column_text(stmt, 0);
                const char *db_time = (const char *)sqlite3_column_text(stmt, 1);
                if (db_days) days_str = db_days;
                if (db_time) time_slot = db_time;
            }
        }
        sqlite3_finalize(stmt);

        // Add toggle buttons for days
        const char *day_names[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
        for (int i = 0; i < 7; i++) {
            GtkWidget *day_button = gtk_toggle_button_new_with_label(day_names[i]);
            gtk_widget_set_size_request(day_button, 50, -1);
            if (strstr(days_str, day_names[i])) {
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(day_button), TRUE);
            }
            g_object_set_data(G_OBJECT(day_button), "app_data", app_data);
            g_object_set_data(G_OBJECT(day_button), "habit_name", g_strdup(habit_name));
            // Store references to all day buttons for this habit
            for (int j = 0; j < 7; j++) {
                g_object_set_data(G_OBJECT(day_button), day_names[j], day_button);
            }
            gtk_box_append(GTK_BOX(row_box), day_button);
        }

        // Add time slot dropdown
        GtkStringList *times_list = gtk_string_list_new(NULL);
        const char *times[] = {
            "00:00-02:00", "02:00-04:00", "04:00-06:00", "06:00-08:00",
            "08:00-10:00", "10:00-12:00", "12:00-14:00", "14:00-16:00",
            "16:00-18:00", "18:00-20:00", "20:00-22:00", "22:00-24:00"
        };
        int selected_time = 0;
        for (int i = 0; i < 12; i++) {
            gtk_string_list_append(times_list, times[i]);
            if (strcmp(time_slot, times[i]) == 0) {
                selected_time = i;
            }
        }
        GtkWidget *hour_dropdown = gtk_drop_down_new(G_LIST_MODEL(times_list), NULL);
        gtk_drop_down_set_selected(GTK_DROP_DOWN(hour_dropdown), selected_time);
        gtk_widget_set_size_request(hour_dropdown, 120, -1);
        g_object_set_data(G_OBJECT(hour_dropdown), "app_data", app_data);
        g_object_set_data(G_OBJECT(hour_dropdown), "habit_name", g_strdup(habit_name));
        // Link the hour_dropdown to the day buttons
        for (int i = 0; i < 7; i++) {
            GtkWidget *day_button = (GtkWidget *)g_object_get_data(G_OBJECT(row_box), day_names[i]);
            if (day_button) {
                g_object_set_data(G_OBJECT(day_button), "hour_dropdown", hour_dropdown);
            }
        }
        g_signal_connect(hour_dropdown, "notify::selected", G_CALLBACK(on_day_toggled), NULL);
        gtk_box_append(GTK_BOX(row_box), hour_dropdown);

        // Add remove button
        GtkWidget *remove_button = gtk_button_new_with_label("Remove");
        g_object_set_data(G_OBJECT(remove_button), "habit_name", g_strdup(habit_name));
        g_object_set_data(G_OBJECT(remove_button), "row", row_box);
        g_signal_connect(remove_button, "clicked", G_CALLBACK(on_remove_habit), app_data);
        gtk_box_append(GTK_BOX(row_box), remove_button);

        gtk_grid_attach(GTK_GRID(habits_grid), row_box, 0, row, 1, 1);
        row++;
    }

    GtkWidget *scrolled_window = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled_window), habits_grid);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_box_append(GTK_BOX(vbox), scrolled_window);

    // Separator
    GtkWidget *separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_append(GTK_BOX(vbox), separator);

    // Add new habit section
    GtkWidget *add_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_box_append(GTK_BOX(vbox), add_box);

    GtkWidget *habit_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(habit_entry), "Enter new habit...");
    gtk_box_append(GTK_BOX(add_box), habit_entry);

    // Days selection for new habit
    GtkWidget *days_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    const char *day_names[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    for (int i = 0; i < 7; i++) {
        GtkWidget *day_button = gtk_toggle_button_new_with_label(day_names[i]);
        g_object_set_data(G_OBJECT(days_box), day_names[i], day_button);
        gtk_box_append(GTK_BOX(days_box), day_button);
    }
    gtk_box_append(GTK_BOX(add_box), days_box);

    // Time slot selection for new habit
    GtkWidget *time_label = gtk_label_new("Time Slot:");
    gtk_widget_set_halign(time_label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(add_box), time_label);

    GtkStringList *times_list = gtk_string_list_new(NULL);
    const char *times[] = {
        "00:00-02:00", "02:00-04:00", "04:00-06:00", "06:00-08:00",
        "08:00-10:00", "10:00-12:00", "12:00-14:00", "14:00-16:00",
        "16:00-18:00", "18:00-20:00", "20:00-22:00", "22:00-24:00"
    };
    for (int i = 0; i < 12; i++) {
        gtk_string_list_append(times_list, times[i]);
    }
    GtkWidget *hour_dropdown = gtk_drop_down_new(G_LIST_MODEL(times_list), NULL);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(hour_dropdown), 0);
    gtk_box_append(GTK_BOX(add_box), hour_dropdown);

    GtkWidget *add_button = gtk_button_new_with_label("Add Habit");
    g_object_set_data(G_OBJECT(add_button), "app_data", app_data);
    g_object_set_data(G_OBJECT(add_button), "entry", habit_entry);
    g_object_set_data(G_OBJECT(add_button), "habits_grid", habits_grid);
    g_object_set_data(G_OBJECT(add_button), "days_box", days_box);
    g_object_set_data(G_OBJECT(add_button), "hour_dropdown", hour_dropdown);
    g_signal_connect(add_button, "clicked", G_CALLBACK(on_add_habit_in_edit), app_data);
    gtk_box_append(GTK_BOX(add_box), add_button);

    // Separator
    GtkWidget *separator2 = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_append(GTK_BOX(vbox), separator2);

    // Save button
    GtkWidget *save_button = gtk_button_new_with_label("Save");
    g_signal_connect_swapped(save_button, "clicked", G_CALLBACK(gtk_window_destroy), edit_window);
    gtk_box_append(GTK_BOX(vbox), save_button);

    gtk_window_present(GTK_WINDOW(edit_window));
}

// Callback to mark a task as done
static void on_mark_task_done(GtkButton *button, AppData *app_data) {
    const char *task = (const char *)g_object_get_data(G_OBJECT(button), "task");
    const char *day = (const char *)g_object_get_data(G_OBJECT(button), "day");
    const char *time_slot = (const char *)g_object_get_data(G_OBJECT(button), "time_slot");
    GtkWidget *task_row = (GtkWidget *)g_object_get_data(G_OBJECT(button), "task_row");
    GtkWidget *task_label = (GtkWidget *)g_object_get_data(G_OBJECT(button), "task_label");

    // Remove from timetable UI
    if (task_label) {
        gtk_box_remove(GTK_BOX(gtk_widget_get_parent(task_label)), task_label);
    }

    // Remove from pending tasks UI
    gtk_box_remove(GTK_BOX(app_data->pending_tasks_box), task_row);

    // Move task from timetable_tasks to completed_tasks in the database
    char insert_query[512];
    snprintf(insert_query, sizeof(insert_query),
             "INSERT INTO completed_tasks (task, day, time_slot) VALUES ('%s', '%s', '%s');",
             task, day, time_slot);
    if (sqlite3_exec(app_data->db, insert_query, NULL, NULL, NULL) == SQLITE_OK) {
        g_print("Task '%s' marked as done\n", task);
    } else {
        g_printerr("Failed to insert task into completed_tasks: %s\n", sqlite3_errmsg(app_data->db));
    }

    char delete_query[512];
    snprintf(delete_query, sizeof(delete_query),
             "DELETE FROM timetable_tasks WHERE task = '%s' AND day = '%s' AND time_slot = '%s';",
             task, day, time_slot);
    if (sqlite3_exec(app_data->db, delete_query, NULL, NULL, NULL) == SQLITE_OK) {
        g_print("Task '%s' removed from timetable_tasks\n", task);
    } else {
        g_printerr("Failed to delete task from timetable_tasks: %s\n", sqlite3_errmsg(app_data->db));
    }

    // Add to completed tasks UI
    char task_display[512];
    snprintf(task_display, sizeof(task_display), "%s (%s, %s)", task, day, time_slot);
    GtkWidget *completed_task_label = gtk_label_new(task_display);
    gtk_widget_set_halign(completed_task_label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(app_data->completed_tasks_box), completed_task_label);

    // Free allocated data
    g_free((char *)task);
    g_free((char *)day);
    g_free((char *)time_slot);
}

// Callback for "OK" button in the add task dialog (Timetable page)
static void on_add_task_ok_clicked(GtkButton *button, gpointer data) {
    struct {
        AppData *app_data;
        const char *day;
        const char *time_slot;
        GtkWidget *task_box;
        GtkWidget *dialog;
        GtkWidget *entry;
    } *task_data = data;

    const char *task = gtk_editable_get_text(GTK_EDITABLE(task_data->entry));
    if (task && strlen(task) > 0) {
        // Add task to database
        char query[512];
        snprintf(query, sizeof(query),
                 "INSERT INTO timetable_tasks (day, time_slot, task) VALUES ('%s', '%s', '%s');",
                 task_data->day, task_data->time_slot, task);
        if (sqlite3_exec(task_data->app_data->db, query, NULL, NULL, NULL) == SQLITE_OK) {
            g_print("Task '%s' added to %s at %s\n", task, task_data->day, task_data->time_slot);

            // Add task to UI in timetable
            GtkWidget *task_label = gtk_label_new(task);
            gtk_widget_set_halign(task_label, GTK_ALIGN_START);
            gtk_box_append(GTK_BOX(task_data->task_box), task_label);

            // Add task to pending tasks on Habits page
            char task_display[512];
            snprintf(task_display, sizeof(task_display), "%s (%s, %s)", task, task_data->day, task_data->time_slot);
            GtkWidget *task_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
            GtkWidget *pending_task_label = gtk_label_new(task_display);
            gtk_widget_set_halign(pending_task_label, GTK_ALIGN_START);
            gtk_box_append(GTK_BOX(task_row), pending_task_label);

            // Add "Mark as Done" button
            GtkWidget *done_button = gtk_button_new_with_label("Mark as Done");
            g_object_set_data(G_OBJECT(done_button), "task", g_strdup(task));
            g_object_set_data(G_OBJECT(done_button), "day", g_strdup(task_data->day));
            g_object_set_data(G_OBJECT(done_button), "time_slot", g_strdup(task_data->time_slot));
            g_object_set_data(G_OBJECT(done_button), "task_row", task_row);
            g_object_set_data(G_OBJECT(done_button), "task_label", task_label);
            g_signal_connect(done_button, "clicked", G_CALLBACK(on_mark_task_done), task_data->app_data);
            gtk_box_append(GTK_BOX(task_row), done_button);

            gtk_box_append(GTK_BOX(task_data->app_data->pending_tasks_box), task_row);
        } else {
            g_printerr("Failed to add task to database: %s\n", sqlite3_errmsg(task_data->app_data->db));
        }
    }

    gtk_window_destroy(GTK_WINDOW(task_data->dialog));
    g_free(task_data);
}

// Callback for "Cancel" button in the add task dialog (Timetable page)
static void on_add_task_cancel_clicked(GtkButton *button, gpointer data) {
    struct {
        AppData *app_data;
        const char *day;
        const char *time_slot;
        GtkWidget *task_box;
        GtkWidget *dialog;
        GtkWidget *entry;
    } *task_data = data;

    gtk_window_destroy(GTK_WINDOW(task_data->dialog));
    g_free(task_data);
}

// Callback for when a timetable cell is clicked
static void on_timetable_cell_clicked(GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y, gpointer data) {
    struct {
        AppData *app_data;
        const char *day;
        const char *time_slot;
        GtkWidget *task_box;
    } *task_data = data;

    // Create a modal window to act as a dialog
    GtkWidget *dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog), "Add Task");
    gtk_window_set_default_size(GTK_WINDOW(dialog), 300, 150);
    gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(task_data->app_data->main_window));
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);

    // Create a vertical box for the dialog content
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(vbox, 10);
    gtk_widget_set_margin_end(vbox, 10);
    gtk_widget_set_margin_top(vbox, 10);
    gtk_widget_set_margin_bottom(vbox, 10);
    gtk_window_set_child(GTK_WINDOW(dialog), vbox);

    // Add a label
    char label_text[64];
    snprintf(label_text, sizeof(label_text), "Add task for %s at %s", task_data->day, task_data->time_slot);
    GtkWidget *label = gtk_label_new(label_text);
    gtk_box_append(GTK_BOX(vbox), label);

    // Add an entry for the task
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Enter task...");
    gtk_box_append(GTK_BOX(vbox), entry);

    // Create a horizontal box for the buttons
    GtkWidget *button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_halign(button_box, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(vbox), button_box);

    // Create "OK" and "Cancel" buttons
    struct {
        AppData *app_data;
        const char *day;
        const char *time_slot;
        GtkWidget *task_box;
        GtkWidget *dialog;
        GtkWidget *entry;
    } *task_dialog_data = g_new0(typeof(*task_dialog_data), 1);
    task_dialog_data->app_data = task_data->app_data;
    task_dialog_data->day = task_data->day;
    task_dialog_data->time_slot = task_data->time_slot;
    task_dialog_data->task_box = task_data->task_box;
    task_dialog_data->dialog = dialog;
    task_dialog_data->entry = entry;

    GtkWidget *ok_button = gtk_button_new_with_label("OK");
    g_signal_connect(ok_button, "clicked", G_CALLBACK(on_add_task_ok_clicked), task_dialog_data);
    gtk_box_append(GTK_BOX(button_box), ok_button);

    GtkWidget *cancel_button = gtk_button_new_with_label("Cancel");
    g_signal_connect(cancel_button, "clicked", G_CALLBACK(on_add_task_cancel_clicked), task_dialog_data);
    gtk_box_append(GTK_BOX(button_box), cancel_button);

    gtk_window_present(GTK_WINDOW(dialog));
}

// Callback to remove a task in the edit tasks window
static void on_remove_task(GtkButton *button, AppData *app_data) {
    const char *task = (const char *)g_object_get_data(G_OBJECT(button), "task");
    const char *day = (const char *)g_object_get_data(G_OBJECT(button), "day");
    const char *time_slot = (const char *)g_object_get_data(G_OBJECT(button), "time_slot");
    GtkWidget *row = (GtkWidget *)g_object_get_data(G_OBJECT(button), "row");

    // Remove from database
    char query[512];
    snprintf(query, sizeof(query),
             "DELETE FROM timetable_tasks WHERE task = '%s' AND day = '%s' AND time_slot = '%s';",
             task, day, time_slot);
    if (sqlite3_exec(app_data->db, query, NULL, NULL, NULL) == SQLITE_OK) {
        g_print("Task '%s' removed from timetable_tasks\n", task);
    } else {
        g_printerr("Failed to delete task from timetable_tasks: %s\n", sqlite3_errmsg(app_data->db));
    }

    // Remove from pending tasks UI
    GtkWidget *child = gtk_widget_get_first_child(app_data->pending_tasks_box);
    while (child) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        GtkWidget *label = gtk_widget_get_first_child(child);
        if (label) {
            const char *label_text = gtk_label_get_text(GTK_LABEL(label));
            char task_display[512];
            snprintf(task_display, sizeof(task_display), "%s (%s, %s)", task, day, time_slot);
            if (strcmp(label_text, task_display) == 0) {
                gtk_box_remove(GTK_BOX(app_data->pending_tasks_box), child);
                break;
            }
        }
        child = next;
    }

    // Remove from timetable UI
    const char *days[] = {"", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    const char *times[] = {
        "00:00-02:00", "02:00-04:00", "04:00-06:00", "06:00-08:00",
        "08:00-10:00", "10:00-12:00", "12:00-14:00", "14:00-16:00",
        "16:00-18:00", "18:00-20:00", "20:00-22:00", "22:00-24:00"
    };
    int day_idx = 0, time_idx = 0;
    for (int i = 1; i <= 7; i++) {
        if (strcmp(days[i], day) == 0) {
            day_idx = i;
            break;
        }
    }
    for (int i = 0; i < 12; i++) {
        if (strcmp(times[i], time_slot) == 0) {
            time_idx = i + 1;
            break;
        }
    }
    if (app_data->timetable_grid) {
        GtkWidget *task_box = gtk_grid_get_child_at(GTK_GRID(app_data->timetable_grid), day_idx, time_idx);
        if (task_box) {
            GtkWidget *child = gtk_widget_get_first_child(task_box);
            while (child) {
                const char *label_text = gtk_label_get_text(GTK_LABEL(child));
                if (strcmp(label_text, task) == 0) {
                    gtk_box_remove(GTK_BOX(task_box), child);
                    break;
                }
                child = gtk_widget_get_next_sibling(child);
            }
        }
    }

    // Remove from edit tasks window
    gtk_grid_remove(GTK_GRID(gtk_widget_get_parent(row)), row);

    // Free allocated data
    g_free((char *)task);
    g_free((char *)day);
    g_free((char *)time_slot);
}

// Callback to add a task in the edit tasks window
static void on_add_task_in_edit(GtkButton *button, gpointer data) {
    AppData *app_data = (AppData *)g_object_get_data(G_OBJECT(button), "app_data");
    GtkWidget *entry = (GtkWidget *)g_object_get_data(G_OBJECT(button), "entry");
    GtkWidget *calendar = (GtkWidget *)g_object_get_data(G_OBJECT(button), "calendar");
    GtkWidget *hour_dropdown = (GtkWidget *)g_object_get_data(G_OBJECT(button), "hour_dropdown");
    GtkWidget *tasks_grid = (GtkWidget *)g_object_get_data(G_OBJECT(button), "tasks_grid");

    GDateTime *date = g_date_time_new_now_local();
    if (!date) return;

    const char *task = gtk_editable_get_text(GTK_EDITABLE(entry));
    if (task && strlen(task) > 0) {
        // Get due date from calendar
        GDateTime *calendar_date = gtk_calendar_get_date(GTK_CALENDAR(calendar));
        if (!calendar_date) {
            g_date_time_unref(date);
            return;
        }

        guint year = g_date_time_get_year(calendar_date);
        guint month = g_date_time_get_month(calendar_date);
        guint day = g_date_time_get_day_of_month(calendar_date);
        g_date_time_unref(calendar_date);

        // Get due time from hour dropdown
        guint selected_hour = gtk_drop_down_get_selected(GTK_DROP_DOWN(hour_dropdown));
        int hour = (int)selected_hour; // Hours are 0 to 23

        // Create GDateTime for the due date and time
        GDateTime *due_date = g_date_time_new_local(year, month, day, hour, 0, 0);
        if (!due_date) {
            g_date_time_unref(date);
            return;
        }

        // Determine the day of the week
        const char *days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        int day_of_week = g_date_time_get_day_of_week(due_date);
        const char *day_str = days[day_of_week - 1]; // GDateTime day of week is 1-based (1=Mon, 7=Sun)

        // Determine the time slot (2-hour intervals)
        const char *times[] = {
            "00:00-02:00", "02:00-04:00", "04:00-06:00", "06:00-08:00",
            "08:00-10:00", "10:00-12:00", "12:00-14:00", "14:00-16:00",
            "16:00-18:00", "18:00-20:00", "20:00-22:00", "22:00-24:00"
        };
        int time_slot_idx = hour / 2;
        const char *time_slot = times[time_slot_idx];

        // Add to database
        char query[512];
        snprintf(query, sizeof(query),
                 "INSERT INTO timetable_tasks (day, time_slot, task) VALUES ('%s', '%s', '%s');",
                 day_str, time_slot, task);
        if (sqlite3_exec(app_data->db, query, NULL, NULL, NULL) == SQLITE_OK) {
            g_print("Task '%s' added to %s at %s\n", task, day_str, time_slot);

            // Add to timetable UI using app_data->timetable_grid
            int day_idx = 0, time_idx = 0;
            for (int i = 1; i <= 7; i++) {
                if (strcmp(days[i], day_str) == 0) {
                    day_idx = i;
                    break;
                }
            }
            for (int i = 0; i < 12; i++) {
                if (strcmp(times[i], time_slot) == 0) {
                    time_idx = i + 1;
                    break;
                }
            }
            if (app_data->timetable_grid) {
                GtkWidget *task_box = gtk_grid_get_child_at(GTK_GRID(app_data->timetable_grid), day_idx, time_idx);
                if (task_box) {
                    GtkWidget *task_label = gtk_label_new(task);
                    gtk_widget_set_halign(task_label, GTK_ALIGN_START);
                    gtk_box_append(GTK_BOX(task_box), task_label);

                    // Add to pending tasks UI
                    char task_display[512];
                    snprintf(task_display, sizeof(task_display), "%s (%s, %s)", task, day_str, time_slot);
                    GtkWidget *task_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
                    GtkWidget *pending_task_label = gtk_label_new(task_display);
                    gtk_widget_set_halign(pending_task_label, GTK_ALIGN_START);
                    gtk_box_append(GTK_BOX(task_row), pending_task_label);

                    // Add "Mark as Done" button
                    GtkWidget *done_button = gtk_button_new_with_label("Mark as Done");
                    g_object_set_data(G_OBJECT(done_button), "task", g_strdup(task));
                    g_object_set_data(G_OBJECT(done_button), "day", g_strdup(day_str));
                    g_object_set_data(G_OBJECT(done_button), "time_slot", g_strdup(time_slot));
                    g_object_set_data(G_OBJECT(done_button), "task_row", task_row);
                    g_object_set_data(G_OBJECT(done_button), "task_label", task_label);
                    g_signal_connect(done_button, "clicked", G_CALLBACK(on_mark_task_done), app_data);
                    gtk_box_append(GTK_BOX(task_row), done_button);

                    gtk_box_append(GTK_BOX(app_data->pending_tasks_box), task_row);
                }
            }

            // Calculate the next row for the grid
            int row = 1; // Start after the header row
            GtkWidget *child = gtk_widget_get_first_child(tasks_grid);
            while (child) {
                row++;
                child = gtk_widget_get_next_sibling(child);
            }

            // Add to edit tasks window
            GtkWidget *row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
            char display[512];
            snprintf(display, sizeof(display), "%s (%s, %s)", task, day_str, time_slot);
            GtkWidget *task_label = gtk_label_new(display);
            gtk_widget_set_size_request(task_label, 200, -1);
            gtk_box_append(GTK_BOX(row_box), task_label);

            GtkWidget *remove_button = gtk_button_new_with_label("Remove");
            g_object_set_data(G_OBJECT(remove_button), "task", g_strdup(task));
            g_object_set_data(G_OBJECT(remove_button), "day", g_strdup(day_str));
            g_object_set_data(G_OBJECT(remove_button), "time_slot", g_strdup(time_slot));
            g_object_set_data(G_OBJECT(remove_button), "row", row_box);
            g_signal_connect(remove_button, "clicked", G_CALLBACK(on_remove_task), app_data);
            gtk_box_append(GTK_BOX(row_box), remove_button);

            gtk_grid_attach(GTK_GRID(tasks_grid), row_box, 0, row, 1, 1);
        } else {
            g_printerr("Failed to add task to database: %s\n", sqlite3_errmsg(app_data->db));
        }

        g_date_time_unref(due_date);
        gtk_editable_set_text(GTK_EDITABLE(entry), "");
        gtk_drop_down_set_selected(GTK_DROP_DOWN(hour_dropdown), 0);
    }

    g_date_time_unref(date);
}

// Callback to open the edit tasks window
static void on_edit_tasks(GtkButton *button, AppData *app_data) {
    GtkWidget *edit_window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(edit_window), "Edit Tasks");
    gtk_window_set_transient_for(GTK_WINDOW(edit_window), GTK_WINDOW(app_data->main_window));
    gtk_window_set_modal(GTK_WINDOW(edit_window), TRUE);
    gtk_window_fullscreen(GTK_WINDOW(edit_window));

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(vbox, 10);
    gtk_widget_set_margin_end(vbox, 10);
    gtk_widget_set_margin_top(vbox, 10);
    gtk_widget_set_margin_bottom(vbox, 10);
    gtk_window_set_child(GTK_WINDOW(edit_window), vbox);

    // Grid to display tasks
    GtkWidget *tasks_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(tasks_grid), 5);
    gtk_grid_set_column_spacing(GTK_GRID(tasks_grid), 5);

    // Header row
    const char *labels[] = {"Task (Day, Time)", "Action"};
    for (int i = 0; i < 2; i++) {
        GtkWidget *label = gtk_label_new(labels[i]);
        gtk_widget_set_halign(label, GTK_ALIGN_START);
        gtk_grid_attach(GTK_GRID(tasks_grid), label, i, 0, 1, 1);
    }

    // Populate existing tasks
    char query[256];
    snprintf(query, sizeof(query), "SELECT task, day, time_slot FROM timetable_tasks;");
    sqlite3_stmt *stmt;
    int row = 1;
    if (sqlite3_prepare_v2(app_data->db, query, -1, &stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *task = (const char *)sqlite3_column_text(stmt, 0);
            const char *day = (const char *)sqlite3_column_text(stmt, 1);
            const char *time_slot = (const char *)sqlite3_column_text(stmt, 2);

            GtkWidget *row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
            char display[512];
            snprintf(display, sizeof(display), "%s (%s, %s)", task, day, time_slot);
            GtkWidget *task_label = gtk_label_new(display);
            gtk_widget_set_size_request(task_label, 200, -1);
            gtk_box_append(GTK_BOX(row_box), task_label);

            GtkWidget *remove_button = gtk_button_new_with_label("Remove");
            g_object_set_data(G_OBJECT(remove_button), "task", g_strdup(task));
            g_object_set_data(G_OBJECT(remove_button), "day", g_strdup(day));
            g_object_set_data(G_OBJECT(remove_button), "time_slot", g_strdup(time_slot));
            g_object_set_data(G_OBJECT(remove_button), "row", row_box);
            g_signal_connect(remove_button, "clicked", G_CALLBACK(on_remove_task), app_data);
            gtk_box_append(GTK_BOX(row_box), remove_button);

            gtk_grid_attach(GTK_GRID(tasks_grid), row_box, 0, row, 1, 1);
            row++;
        }
    }
    sqlite3_finalize(stmt);

    GtkWidget *scrolled_window = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled_window), tasks_grid);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scrolled_window, TRUE);
    gtk_widget_set_size_request(scrolled_window, -1, 300); // Increase minimum height
    gtk_box_append(GTK_BOX(vbox), scrolled_window);

    // Separator
    GtkWidget *separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_append(GTK_BOX(vbox), separator);

    // Add new task section
    GtkWidget *add_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_box_append(GTK_BOX(vbox), add_box);

    GtkWidget *task_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(task_entry), "Enter new task...");
    gtk_box_append(GTK_BOX(add_box), task_entry);

    // Due date selection
    GtkWidget *calendar_label = gtk_label_new("Due Date:");
    gtk_widget_set_halign(calendar_label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(add_box), calendar_label);

    GtkWidget *calendar = gtk_calendar_new();
    gtk_box_append(GTK_BOX(add_box), calendar);

    // Due time selection (dropdown for hours)
    GtkWidget *time_label = gtk_label_new("Due Time (Hour):");
    gtk_widget_set_halign(time_label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(add_box), time_label);

    // Create a string list for hours (00 to 23)
    GtkStringList *hours_list = gtk_string_list_new(NULL);
    for (int i = 0; i < 24; i++) {
        char hour[3];
        snprintf(hour, sizeof(hour), "%02d", i);
        gtk_string_list_append(hours_list, hour);
    }

    // Create a dropdown with the hours list
    GtkWidget *hour_dropdown = gtk_drop_down_new(G_LIST_MODEL(hours_list), NULL);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(hour_dropdown), 0);
    gtk_box_append(GTK_BOX(add_box), hour_dropdown);

    GtkWidget *add_button = gtk_button_new_with_label("Add Task");
    g_object_set_data(G_OBJECT(add_button), "app_data", app_data);
    g_object_set_data(G_OBJECT(add_button), "entry", task_entry);
    g_object_set_data(G_OBJECT(add_button), "calendar", calendar);
    g_object_set_data(G_OBJECT(add_button), "hour_dropdown", hour_dropdown);
    g_object_set_data(G_OBJECT(add_button), "tasks_grid", tasks_grid);
    g_signal_connect(add_button, "clicked", G_CALLBACK(on_add_task_in_edit), app_data);
    gtk_box_append(GTK_BOX(add_box), add_button);

    // Separator
    GtkWidget *separator2 = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_append(GTK_BOX(vbox), separator2);

    // Save button
    GtkWidget *save_button = gtk_button_new_with_label("Save");
    g_signal_connect_swapped(save_button, "clicked", G_CALLBACK(gtk_window_destroy), edit_window);
    gtk_box_append(GTK_BOX(vbox), save_button);

    gtk_window_present(GTK_WINDOW(edit_window));
}

// Create the timetable page
static GtkWidget *create_timetable_page(AppData *app_data) {
    // Create CSS provider for styling the grid slots and habit labels
    GtkCssProvider *css_provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css_provider,
                                      ".task-slot { border: 1px solid #ccc; background-color: #f9f9f9; padding: 5px; }"
                                      ".habit-label { color: blue; }");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                               GTK_STYLE_PROVIDER(css_provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 2);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 2);
    gtk_widget_set_margin_start(grid, 10);
    gtk_widget_set_margin_end(grid, 10);
    gtk_widget_set_margin_top(grid, 10);
    gtk_widget_set_margin_bottom(grid, 10);
    gtk_widget_set_hexpand(grid, TRUE);
    gtk_widget_set_vexpand(grid, TRUE);

    // Store the grid in app_data
    app_data->timetable_grid = grid;

    // Days of the week as column headers
    const char *days[] = {"", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    for (int i = 0; i < 8; i++) {
        GtkWidget *label = gtk_label_new(days[i]);
        gtk_widget_set_halign(label, GTK_ALIGN_CENTER);
        gtk_grid_attach(GTK_GRID(grid), label, i, 0, 1, 1);
    }

    // Time intervals as row headers (00:00 to 24:00 in 2-hour intervals)
    const char *times[] = {
        "00:00-02:00", "02:00-04:00", "04:00-06:00", "06:00-08:00",
        "08:00-10:00", "10:00-12:00", "12:00-14:00", "14:00-16:00",
        "16:00-18:00", "18:00-20:00", "20:00-22:00", "22:00-24:00"
    };
    for (int i = 0; i < 12; i++) {
        GtkWidget *label = gtk_label_new(times[i]);
        gtk_widget_set_halign(label, GTK_ALIGN_START);
        gtk_grid_attach(GTK_GRID(grid), label, 0, i + 1, 1, 1);
    }

    // Add cells for the timetable
    for (int day = 1; day <= 7; day++) {
        for (int time = 1; time <= 12; time++) {
            // Create a box to hold tasks
            GtkWidget *task_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
            gtk_widget_set_hexpand(task_box, TRUE);
            gtk_widget_set_vexpand(task_box, TRUE);

            // Apply CSS class for styling
            gtk_widget_add_css_class(task_box, "task-slot");

            // Load existing tasks from the database
            char query[256];
            snprintf(query, sizeof(query),
                     "SELECT task FROM timetable_tasks WHERE day = '%s' AND time_slot = '%s';",
                     days[day], times[time - 1]);
            sqlite3_stmt *stmt;
            if (sqlite3_prepare_v2(app_data->db, query, -1, &stmt, NULL) == SQLITE_OK) {
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    const char *task = (const char *)sqlite3_column_text(stmt, 0);
                    GtkWidget *task_label = gtk_label_new(task);
                    gtk_widget_set_halign(task_label, GTK_ALIGN_START);
                    gtk_box_append(GTK_BOX(task_box), task_label);

                    // Also add to pending tasks on Habits page
                    char task_display[512];
                    snprintf(task_display, sizeof(task_display), "%s (%s, %s)", task, days[day], times[time - 1]);
                    GtkWidget *task_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
                    GtkWidget *pending_task_label = gtk_label_new(task_display);
                    gtk_widget_set_halign(pending_task_label, GTK_ALIGN_START);
                    gtk_box_append(GTK_BOX(task_row), pending_task_label);

                    // Add "Mark as Done" button
                    GtkWidget *done_button = gtk_button_new_with_label("Mark as Done");
                    g_object_set_data(G_OBJECT(done_button), "task", g_strdup(task));
                    g_object_set_data(G_OBJECT(done_button), "day", g_strdup(days[day]));
                    g_object_set_data(G_OBJECT(done_button), "time_slot", g_strdup(times[time - 1]));
                    g_object_set_data(G_OBJECT(done_button), "task_row", task_row);
                    g_object_set_data(G_OBJECT(done_button), "task_label", task_label);
                    g_signal_connect(done_button, "clicked", G_CALLBACK(on_mark_task_done), app_data);
                    gtk_box_append(GTK_BOX(task_row), done_button);

                    gtk_box_append(GTK_BOX(app_data->pending_tasks_box), task_row);
                }
            }
            sqlite3_finalize(stmt);

            // Load habits for this day and time slot
            char habit_query[256];
            snprintf(habit_query, sizeof(habit_query),
                     "SELECT habit_name, days, time_slot FROM habit_tracking WHERE time_slot = '%s';",
                     times[time - 1]);
            sqlite3_stmt *habit_stmt;
            if (sqlite3_prepare_v2(app_data->db, habit_query, -1, &habit_stmt, NULL) == SQLITE_OK) {
                while (sqlite3_step(habit_stmt) == SQLITE_ROW) {
                    const char *habit_name = (const char *)sqlite3_column_text(habit_stmt, 0);
                    const char *habit_days = (const char *)sqlite3_column_text(habit_stmt, 1);

                    // Check if the current day is in the habit's days
                    if (habit_days && strstr(habit_days, days[day])) {
                        GtkWidget *habit_label = gtk_label_new(habit_name);
                        gtk_widget_set_halign(habit_label, GTK_ALIGN_START);
                        gtk_widget_add_css_class(habit_label, "habit-label");
                        gtk_box_append(GTK_BOX(task_box), habit_label);
                    }
                }
            }
            sqlite3_finalize(habit_stmt);

            // Make the cell clickable
            GtkGestureClick *event_controller = GTK_GESTURE_CLICK(gtk_gesture_click_new());
            struct {
                AppData *app_data;
                const char *day;
                const char *time_slot;
                GtkWidget *task_box;
            } *task_data = g_new0(typeof(*task_data), 1);
            task_data->app_data = app_data;
            task_data->day = g_strdup(days[day]);
            task_data->time_slot = g_strdup(times[time - 1]);
            task_data->task_box = task_box;

            g_signal_connect(event_controller, "pressed", G_CALLBACK(on_timetable_cell_clicked), task_data);
            gtk_widget_add_controller(task_box, GTK_EVENT_CONTROLLER(event_controller));

            gtk_grid_attach(GTK_GRID(grid), task_box, day, time, 1, 1);
        }
    }

    // Load completed tasks for Habits page
    char completed_query[256];
    snprintf(completed_query, sizeof(completed_query), "SELECT task, day, time_slot FROM completed_tasks;");
    sqlite3_stmt *completed_stmt;
    if (sqlite3_prepare_v2(app_data->db, completed_query, -1, &completed_stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(completed_stmt) == SQLITE_ROW) {
            const char *task = (const char *)sqlite3_column_text(completed_stmt, 0);
            const char *day = (const char *)sqlite3_column_text(completed_stmt, 1);
            const char *time_slot = (const char *)sqlite3_column_text(completed_stmt, 2);

            char task_display[512];
            snprintf(task_display, sizeof(task_display), "%s (%s, %s)", task, day, time_slot);
            GtkWidget *completed_task_label = gtk_label_new(task_display);
            gtk_widget_set_halign(completed_task_label, GTK_ALIGN_START);
            gtk_box_append(GTK_BOX(app_data->completed_tasks_box), completed_task_label);
        }
    }
    sqlite3_finalize(completed_stmt);

    return grid;
}

// Cleanup function for AppData
static void cleanup_app_data(AppData *app_data) {
    // Free habits list
    g_list_free_full(app_data->habits, g_free);
    app_data->habits = NULL;

    // Free habit_widgets list (widgets are already unparented)
    g_list_free(app_data->habit_widgets);
    app_data->habit_widgets = NULL;

    // Close database
    if (app_data->db) {
        sqlite3_close(app_data->db);
        app_data->db = NULL;
    }

    g_free(app_data);
}

static void on_activate(GtkApplication *app, gpointer user_data) {
    AppData *app_data = g_new0(AppData, 1);
    app_data->habits = NULL;
    app_data->habit_widgets = NULL;
    app_data->timetable_grid = NULL; // Initialize timetable_grid

    // Initialize SQLite database
    if (sqlite3_open("habit_tracker.db", &app_data->db) != SQLITE_OK) {
        g_printerr("Cannot open database: %s\n", sqlite3_errmsg(app_data->db));
        cleanup_app_data(app_data);
        return;
    }

    // Drop the existing tables to clear old data
    const char *drop_habits_table_sql = "DROP TABLE IF EXISTS habit_tracking;";
    if (sqlite3_exec(app_data->db, drop_habits_table_sql, NULL, NULL, NULL) != SQLITE_OK) {
        g_printerr("Failed to drop habit_tracking table: %s\n", sqlite3_errmsg(app_data->db));
        cleanup_app_data(app_data);
        return;
    }
    g_print("Dropped existing habit_tracking table\n");

    const char *drop_tasks_table_sql = "DROP TABLE IF EXISTS timetable_tasks;";
    if (sqlite3_exec(app_data->db, drop_tasks_table_sql, NULL, NULL, NULL) != SQLITE_OK) {
        g_printerr("Failed to drop timetable_tasks table: %s\n", sqlite3_errmsg(app_data->db));
        cleanup_app_data(app_data);
        return;
    }
    g_print("Dropped existing timetable_tasks table\n");

    const char *drop_completed_tasks_table_sql = "DROP TABLE IF EXISTS completed_tasks;";
    if (sqlite3_exec(app_data->db, drop_completed_tasks_table_sql, NULL, NULL, NULL) != SQLITE_OK) {
        g_printerr("Failed to drop completed_tasks table: %s\n", sqlite3_errmsg(app_data->db));
        cleanup_app_data(app_data);
        return;
    }
    g_print("Dropped existing completed_tasks table\n");

    // Create the habits table with time_slot column
    const char *create_habits_table_sql =
        "CREATE TABLE IF NOT EXISTS habit_tracking ("
        "date TEXT, habit_name TEXT, completed INTEGER, "
        "days TEXT, time_slot TEXT, "
        "PRIMARY KEY (date, habit_name));";
    if (sqlite3_exec(app_data->db, create_habits_table_sql, NULL, NULL, NULL) != SQLITE_OK) {
        g_printerr("Failed to create habit_tracking table: %s\n", sqlite3_errmsg(app_data->db));
        cleanup_app_data(app_data);
        return;
    }

    // Create the timetable tasks table
    const char *create_tasks_table_sql =
        "CREATE TABLE IF NOT EXISTS timetable_tasks ("
        "day TEXT, time_slot TEXT, task TEXT, "
        "PRIMARY KEY (day, time_slot, task));";
    if (sqlite3_exec(app_data->db, create_tasks_table_sql, NULL, NULL, NULL) != SQLITE_OK) {
        g_printerr("Failed to create timetable_tasks table: %s\n", sqlite3_errmsg(app_data->db));
        cleanup_app_data(app_data);
        return;
    }

    // Create the completed tasks table
    const char *create_completed_tasks_table_sql =
        "CREATE TABLE IF NOT EXISTS completed_tasks ("
        "task TEXT, day TEXT, time_slot TEXT, "
        "PRIMARY KEY (task, day, time_slot));";
    if (sqlite3_exec(app_data->db, create_completed_tasks_table_sql, NULL, NULL, NULL) != SQLITE_OK) {
        g_printerr("Failed to create completed_tasks table: %s\n", sqlite3_errmsg(app_data->db));
        cleanup_app_data(app_data);
        return;
    }

    // Create the main window
    app_data->main_window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(app_data->main_window), "Habit Tracker");
    gtk_window_set_default_size(GTK_WINDOW(app_data->main_window), 800, 600);

    // Create a notebook for tabs
    GtkWidget *notebook = gtk_notebook_new();
    gtk_window_set_child(GTK_WINDOW(app_data->main_window), notebook);

    // Habits page
    GtkWidget *habits_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(habits_page, 10);
    gtk_widget_set_margin_end(habits_page, 10);
    gtk_widget_set_margin_top(habits_page, 10);
    gtk_widget_set_margin_bottom(habits_page, 10);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), habits_page, gtk_label_new("Habits"));

    // Pending tasks section
    GtkWidget *pending_tasks_label = gtk_label_new("Pending Tasks:");
    gtk_widget_set_halign(pending_tasks_label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(habits_page), pending_tasks_label);

    app_data->pending_tasks_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_box_append(GTK_BOX(habits_page), app_data->pending_tasks_box);

    // Separator
    GtkWidget *separator1 = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_append(GTK_BOX(habits_page), separator1);

    // Completed tasks section
    GtkWidget *completed_tasks_label = gtk_label_new("Completed Tasks:");
    gtk_widget_set_halign(completed_tasks_label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(habits_page), completed_tasks_label);

    app_data->completed_tasks_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_box_append(GTK_BOX(habits_page), app_data->completed_tasks_box);

    // Separator
    GtkWidget *separator2 = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_append(GTK_BOX(habits_page), separator2);

    // Habits section
    GtkWidget *habits_label = gtk_label_new("Your Habits:");
    gtk_widget_set_halign(habits_label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(habits_page), habits_label);

    app_data->habits_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_hexpand(app_data->habits_box, TRUE);
    gtk_box_append(GTK_BOX(habits_page), app_data->habits_box);

    // Load habits from database
    char query[256];
    snprintf(query, sizeof(query), "SELECT habit_name FROM habit_tracking WHERE date = '' OR date IS NULL;");
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(app_data->db, query, -1, &stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *habit_name = (const char *)sqlite3_column_text(stmt, 0);
            app_data->habits = g_list_append(app_data->habits, g_strdup(habit_name));

            // Create habit widget
            GtkWidget *habit_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
            GtkWidget *drawing_area = gtk_drawing_area_new();
            gtk_widget_set_size_request(drawing_area, 60, 60);
            gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(drawing_area), draw_habit_logo, g_strdup(habit_name), g_free);
            g_object_set_data(G_OBJECT(drawing_area), "app_data", app_data);
            g_object_set_data(G_OBJECT(drawing_area), "habit_name", g_strdup(habit_name));
            gtk_box_append(GTK_BOX(habit_box), drawing_area);

            // Set habit_name on the habit_box itself
            g_object_set_data(G_OBJECT(habit_box), "habit_name", g_strdup(habit_name));

            GtkWidget *label = gtk_label_new(habit_name);
            gtk_widget_set_halign(label, GTK_ALIGN_CENTER);
            gtk_box_append(GTK_BOX(habit_box), label);

            gtk_box_append(GTK_BOX(app_data->habits_box), habit_box);
            app_data->habit_widgets = g_list_append(app_data->habit_widgets, habit_box);
        }
    }
    sqlite3_finalize(stmt);

    // Edit habits button
    GtkWidget *edit_habits_button = gtk_button_new_with_label("Edit Habits");
    g_signal_connect(edit_habits_button, "clicked", G_CALLBACK(on_edit_habits), app_data);
    gtk_box_append(GTK_BOX(habits_page), edit_habits_button);

    // Timetable page
    GtkWidget *timetable_page = create_timetable_page(app_data);
    GtkWidget *timetable_scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(timetable_scrolled), timetable_page);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(timetable_scrolled),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), timetable_scrolled, gtk_label_new("Timetable"));

    // Edit tasks button on Timetable page
    GtkWidget *edit_tasks_button = gtk_button_new_with_label("Edit Tasks");
    g_signal_connect(edit_tasks_button, "clicked", G_CALLBACK(on_edit_tasks), app_data);
    gtk_box_append(GTK_BOX(habits_page), edit_tasks_button); // Adding to habits page for visibility

    // Show the window
    gtk_window_present(GTK_WINDOW(app_data->main_window));
}

int main(int argc, char *argv[]) {
    GtkApplication *app = gtk_application_new("org.example.habittracker", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}