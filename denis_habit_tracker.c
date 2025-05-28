#include <gtk/gtk.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    GtkWidget *main_window;
    GtkWidget *habits_vbox;
    GList *habits;
    GList *habit_widgets;
    sqlite3 *db;
    GtkWidget *habits_box;
    GtkWidget *pending_tasks_box;
    GtkWidget *completed_tasks_box;
    GtkWidget *timetable_grid;
} AppData;

// Forward declaration
static void on_done_today_clicked(GtkButton *button, AppData *app_data);


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

static void draw_habit_logo(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data) {
    const char *habit_name = (const char *)data;
    AppData *app_data = g_object_get_data(G_OBJECT(area), "app_data");
    int days = get_days_completed(app_data->db, habit_name);

    int radius = MIN(width, height) / 2 - 5;

    cairo_arc(cr, width / 2, height / 2, radius, 0, 2 * G_PI);
    cairo_set_source_rgb(cr, 0.25, 0.65, 0.25);
    cairo_fill(cr);

    char days_str[16];
    snprintf(days_str, sizeof(days_str), "%d", days);
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 20);
    cairo_text_extents_t extents;
    cairo_text_extents(cr, days_str, &extents);
    cairo_move_to(cr, width / 2 - extents.width / 2, height / 2 + extents.height / 2);
    cairo_show_text(cr, days_str);
}

static void on_remove_habit(GtkButton *button, AppData *app_data) {
    const char *habit_name = (const char *)g_object_get_data(G_OBJECT(button), "habit_name");
    GtkWidget *row = (GtkWidget *)g_object_get_data(G_OBJECT(button), "row");

    if (!habit_name || !row) {
        g_printerr("Error: Invalid habit_name or row in on_remove_habit\n");
        return;
    }

    GList *link = g_list_find_custom(app_data->habits, habit_name, (GCompareFunc)strcmp);
    if (link) {
        g_free(link->data);
        app_data->habits = g_list_delete_link(app_data->habits, link);
    } else {
        g_printerr("Error: Habit %s not found in habits list\n", habit_name);
    }

    gtk_grid_remove(GTK_GRID(gtk_widget_get_parent(row)), row);
    gtk_widget_unparent(row);

    GList *iter = app_data->habit_widgets;
    gboolean found = FALSE;
    while (iter) {
        GtkWidget *widget = (GtkWidget *)iter->data;
        const char *name = (const char *)g_object_get_data(G_OBJECT(widget), "habit_name");
        if (name && strcmp(name, habit_name) == 0) {
            gtk_box_remove(GTK_BOX(app_data->habits_box), widget);
            app_data->habit_widgets = g_list_remove(app_data->habit_widgets, widget);
            g_free((char *)name);
            gtk_widget_unparent(widget);
            found = TRUE;
            break;
        }
        iter = iter->next;
    }

    if (!found) {
        GtkWidget *child = gtk_widget_get_first_child(app_data->habits_box);
        while (child) {
            const char *name = (const char *)g_object_get_data(G_OBJECT(child), "habit_name");
            if (name && strcmp(name, habit_name) == 0) {
                gtk_box_remove(GTK_BOX(app_data->habits_box), child);
                g_free((char *)name);
                gtk_widget_unparent(child);
                found = TRUE;
                break;
            }

            GtkWidget *drawing_area = gtk_widget_get_first_child(child);
            if (drawing_area) {
                name = (const char *)g_object_get_data(G_OBJECT(drawing_area), "habit_name");
                if (name && strcmp(name, habit_name) == 0) {
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
        gtk_widget_queue_draw(app_data->habits_box);
    } else {
        g_printerr("Error: Habit widget %s not found in habits_box\n", habit_name);
    }

    const char *days[] = {"", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    const char *times[] = {
        "00:00-02:00", "02:00-04:00", "04:00-06:00", "06:00-08:00",
        "08:00-10:00", "10:00-12:00", "12:00-14:00", "14:00-16:00",
        "16:00-18:00", "18:00-20:00", "20:00-22:00", "22:00-24:00"
    };
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
                        if (GTK_IS_LABEL(child)) {
                            const char *label_text = gtk_label_get_text(GTK_LABEL(child));
                            if (strcmp(label_text, habit_name) == 0) {
                                gtk_box_remove(GTK_BOX(task_box), child);
                                break;
                            }
                        }
                        child = gtk_widget_get_next_sibling(child);
                    }
                }
            }
            token = strtok(NULL, ",");
        }
        g_free(days_copy);
    }

    snprintf(query, sizeof(query), "DELETE FROM habit_tracking WHERE habit_name = '%s';", habit_name);
    sqlite3_exec(app_data->db, query, NULL, NULL, NULL);
}

static void on_day_toggled(GtkToggleButton *toggle_button, gpointer data) {
    AppData *app_data = (AppData *)g_object_get_data(G_OBJECT(toggle_button), "app_data");
    const char *habit_name = (const char *)g_object_get_data(G_OBJECT(toggle_button), "habit_name");
    GtkWidget *hour_dropdown_generic = (GtkWidget *)g_object_get_data(G_OBJECT(toggle_button), "hour_dropdown");

    GtkDropDown* hour_dropdown = GTK_DROP_DOWN(hour_dropdown_generic);


    GString *days = g_string_new("");
    const char *day_names[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};

    GtkWidget *parent_box = gtk_widget_get_parent(GTK_WIDGET(toggle_button));
    if (!GTK_IS_BOX(parent_box)) {
         parent_box = gtk_widget_get_parent(gtk_widget_get_parent(GTK_WIDGET(toggle_button)));
    }


    GtkWidget *child = gtk_widget_get_first_child(parent_box);
    int current_button_idx = 0;
    while(child) {
        if(GTK_IS_TOGGLE_BUTTON(child)) {
             if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(child))) {
                if (days->len > 0) g_string_append(days, ",");
                g_string_append(days, day_names[current_button_idx % 7]);
            }
            current_button_idx++;
        }
        if(child == GTK_WIDGET(toggle_button) && hour_dropdown == NULL){

            GtkWidget* next_sibling = gtk_widget_get_next_sibling(child);
            while(next_sibling && !GTK_IS_DROP_DOWN(next_sibling)){
                 next_sibling = gtk_widget_get_next_sibling(next_sibling);
            }
            if(GTK_IS_DROP_DOWN(next_sibling)){
                hour_dropdown = GTK_DROP_DOWN(next_sibling);
            }
        }
         child = gtk_widget_get_next_sibling(child);
         if(current_button_idx >=7 && hour_dropdown != NULL) break;
    }


    const char *times[] = {
        "00:00-02:00", "02:00-04:00", "04:00-06:00", "06:00-08:00",
        "08:00-10:00", "10:00-12:00", "12:00-14:00", "14:00-16:00",
        "16:00-18:00", "18:00-20:00", "20:00-22:00", "22:00-24:00"
    };
    guint selected_time = 0;
    if(hour_dropdown){
        selected_time = gtk_drop_down_get_selected(hour_dropdown);
    }
    const char *time_slot = times[selected_time];

    const char *day_labels[] = {"", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    for (int day = 1; day <= 7; day++) {
        for (int time_idx_loop = 1; time_idx_loop <= 12; time_idx_loop++) {
            if (!app_data->timetable_grid) continue;
            GtkWidget *task_box = gtk_grid_get_child_at(GTK_GRID(app_data->timetable_grid), day, time_idx_loop);
            if (task_box) {
                GtkWidget *current_child = gtk_widget_get_first_child(task_box);
                while (current_child) {
                    GtkWidget *next_child = gtk_widget_get_next_sibling(current_child);
                    if (GTK_IS_LABEL(current_child)) {
                        const char *label_text = gtk_label_get_text(GTK_LABEL(current_child));
                        if (strcmp(label_text, habit_name) == 0) {
                            gtk_box_remove(GTK_BOX(task_box), current_child);
                        }
                    }
                    current_child = next_child;
                }
            }
        }
    }

    int time_idx_timetable = selected_time + 1;
    char *days_copy = g_strdup(days->str);
    char *token = strtok(days_copy, ",");
    while (token) {
        int day_idx = 0;
        for (int i = 0; i < 7; i++) {
            if (strcmp(day_names[i], token) == 0) {
                day_idx = i + 1;
                break;
            }
        }
        if (day_idx > 0 && app_data->timetable_grid) {
            GtkWidget *task_box = gtk_grid_get_child_at(GTK_GRID(app_data->timetable_grid), day_idx, time_idx_timetable);
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

    char query[256];
    snprintf(query, sizeof(query),
             "UPDATE habit_tracking SET days = '%s', time_slot = '%s' WHERE habit_name = '%s';",
             days->str, time_slot, habit_name);
    if (sqlite3_exec(app_data->db, query, NULL, NULL, NULL) != SQLITE_OK) {
        g_printerr("Failed to update days and time for habit %s: %s\n", habit_name, sqlite3_errmsg(app_data->db));
    }

    g_string_free(days, TRUE);
}


static void on_add_habit_in_edit(GtkButton *button, gpointer data) {
    AppData *app_data = (AppData *)g_object_get_data(G_OBJECT(button), "app_data");
    GtkWidget *entry = (GtkWidget *)g_object_get_data(G_OBJECT(button), "entry");
    GtkWidget *habits_grid = (GtkWidget *)g_object_get_data(G_OBJECT(button), "habits_grid");
    GtkWidget *days_box_container = (GtkWidget *)g_object_get_data(G_OBJECT(button), "days_box");
    GtkWidget *hour_dropdown_widget = (GtkWidget *)g_object_get_data(G_OBJECT(button), "hour_dropdown");

    const char *times[] = {
        "00:00-02:00", "02:00-04:00", "04:00-06:00", "06:00-08:00",
        "08:00-10:00", "10:00-12:00", "12:00-14:00", "14:00-16:00",
        "16:00-18:00", "18:00-20:00", "20:00-22:00", "22:00-24:00"
    };

    const char *habit_name = gtk_editable_get_text(GTK_EDITABLE(entry));
    if (habit_name && strlen(habit_name) > 0) {
        GString *days_str_g = g_string_new("");
        const char *day_names[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
        GtkWidget *child_day_button = gtk_widget_get_first_child(days_box_container);
        int day_idx_loop = 0;
        while(child_day_button && day_idx_loop < 7){
            if(GTK_IS_TOGGLE_BUTTON(child_day_button)){
                if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(child_day_button))) {
                    if (days_str_g->len > 0) g_string_append(days_str_g, ",");
                    g_string_append(days_str_g, day_names[day_idx_loop]);
                }
                day_idx_loop++;
            }
            child_day_button = gtk_widget_get_next_sibling(child_day_button);
        }


        guint selected_time = gtk_drop_down_get_selected(GTK_DROP_DOWN(hour_dropdown_widget));
        const char *time_slot = times[selected_time];

        app_data->habits = g_list_append(app_data->habits, g_strdup(habit_name));

        GtkWidget *habit_box_main = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
        GtkWidget *drawing_area = gtk_drawing_area_new();
        gtk_widget_set_size_request(drawing_area, 60, 60);
        gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(drawing_area), draw_habit_logo, g_strdup(habit_name), g_free);
        g_object_set_data(G_OBJECT(drawing_area), "app_data", app_data);
        g_object_set_data(G_OBJECT(drawing_area), "habit_name", g_strdup(habit_name));
        gtk_box_append(GTK_BOX(habit_box_main), drawing_area);

        g_object_set_data(G_OBJECT(habit_box_main), "habit_name", g_strdup(habit_name));

        GtkWidget *label_main = gtk_label_new(habit_name);
        gtk_widget_set_halign(label_main, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(habit_box_main), label_main);

        GDateTime *today = g_date_time_new_now_local();
        int day_of_week_today = g_date_time_get_day_of_week(today);
        int adjusted_day_today = (day_of_week_today == 7) ? 6 : (day_of_week_today - 1);
        const char *current_day_str = day_names[adjusted_day_today];
        g_date_time_unref(today);

        if (strstr(days_str_g->str, current_day_str)) {
            GtkWidget *done_button = gtk_button_new_with_label("Done Today");
            g_object_set_data(G_OBJECT(done_button), "habit_name", g_strdup(habit_name));
            g_object_set_data(G_OBJECT(done_button), "drawing_area", drawing_area);
            g_signal_connect(done_button, "clicked", G_CALLBACK(on_done_today_clicked), app_data);
            gtk_box_append(GTK_BOX(habit_box_main), done_button);
        }


        gtk_box_append(GTK_BOX(app_data->habits_box), habit_box_main);
        app_data->habit_widgets = g_list_append(app_data->habit_widgets, habit_box_main);

        char query[256];
        snprintf(query, sizeof(query),
                 "INSERT OR IGNORE INTO habit_tracking (date, habit_name, completed, days, time_slot) VALUES ('', '%s', 0, '%s', '%s');",
                 habit_name, days_str_g->str, time_slot);
        if (sqlite3_exec(app_data->db, query, NULL, NULL, NULL) != SQLITE_OK) {
            g_printerr("Failed to add habit to database: %s\n", sqlite3_errmsg(app_data->db));
        }

        int time_idx_timetable = selected_time + 1;
        char *days_copy = g_strdup(days_str_g->str);
        char *token = strtok(days_copy, ",");
        while (token) {
            int day_idx_timetable = 0;
            for (int i = 0; i < 7; i++) {
                if (strcmp(day_names[i], token) == 0) {
                    day_idx_timetable = i + 1;
                    break;
                }
            }
            if (day_idx_timetable > 0 && app_data->timetable_grid) {
                GtkWidget *task_box = gtk_grid_get_child_at(GTK_GRID(app_data->timetable_grid), day_idx_timetable, time_idx_timetable);
                if (task_box) {
                    GtkWidget *habit_label_timetable = gtk_label_new(habit_name);
                    gtk_widget_set_halign(habit_label_timetable, GTK_ALIGN_START);
                    gtk_widget_add_css_class(habit_label_timetable, "habit-label");
                    gtk_box_append(GTK_BOX(task_box), habit_label_timetable);
                }
            }
            token = strtok(NULL, ",");
        }
        g_free(days_copy);

        int next_row_grid = 0;
        GtkWidget *grid_child = gtk_widget_get_first_child(habits_grid);
        while(grid_child){
            next_row_grid++;
            grid_child = gtk_widget_get_next_sibling(grid_child);
        }
        if(next_row_grid == 0) next_row_grid = 1;


        GtkWidget *row_box_edit = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
        GtkWidget *row_label_edit = gtk_label_new(habit_name);
        gtk_widget_set_size_request(row_label_edit, 100, -1);
        gtk_box_append(GTK_BOX(row_box_edit), row_label_edit);

        GtkWidget* day_buttons_in_row[7];

        for (int i = 0; i < 7; i++) {
            GtkWidget *day_button_edit = gtk_toggle_button_new_with_label(day_names[i]);
            gtk_widget_set_size_request(day_button_edit, 50, -1);
            if (strstr(days_str_g->str, day_names[i])) {
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(day_button_edit), TRUE);
            }
            g_object_set_data(G_OBJECT(day_button_edit), "app_data", app_data);
            g_object_set_data(G_OBJECT(day_button_edit), "habit_name", g_strdup(habit_name));
            day_buttons_in_row[i] = day_button_edit;
            gtk_box_append(GTK_BOX(row_box_edit), day_button_edit);
        }

        GtkStringList *times_list_edit = gtk_string_list_new(NULL);
        for (int i = 0; i < 12; i++) {
            gtk_string_list_append(times_list_edit, times[i]);
        }
        GtkWidget *row_hour_dropdown_edit = gtk_drop_down_new(G_LIST_MODEL(times_list_edit), NULL);
        gtk_drop_down_set_selected(GTK_DROP_DOWN(row_hour_dropdown_edit), selected_time);
        gtk_widget_set_size_request(row_hour_dropdown_edit, 120, -1);
        g_object_set_data(G_OBJECT(row_hour_dropdown_edit), "app_data", app_data);
        g_object_set_data(G_OBJECT(row_hour_dropdown_edit), "habit_name", g_strdup(habit_name));

        for(int i=0; i<7; ++i){
            g_object_set_data(G_OBJECT(day_buttons_in_row[i]), "hour_dropdown", row_hour_dropdown_edit);
            g_signal_connect(day_buttons_in_row[i], "toggled", G_CALLBACK(on_day_toggled), app_data);
        }
        g_signal_connect(row_hour_dropdown_edit, "notify::selected", G_CALLBACK(on_day_toggled), app_data);


        gtk_box_append(GTK_BOX(row_box_edit), row_hour_dropdown_edit);

        GtkWidget *remove_button_edit = gtk_button_new_with_label("Remove");
        g_object_set_data(G_OBJECT(remove_button_edit), "habit_name", g_strdup(habit_name));
        g_object_set_data(G_OBJECT(remove_button_edit), "row", row_box_edit);
        g_signal_connect(remove_button_edit, "clicked", G_CALLBACK(on_remove_habit), app_data);
        gtk_box_append(GTK_BOX(row_box_edit), remove_button_edit);

        gtk_grid_attach(GTK_GRID(habits_grid), row_box_edit, 0, next_row_grid, 10, 1);


        gtk_editable_set_text(GTK_EDITABLE(entry), "");
        child_day_button = gtk_widget_get_first_child(days_box_container);
        day_idx_loop = 0;
         while(child_day_button && day_idx_loop < 7){
            if(GTK_IS_TOGGLE_BUTTON(child_day_button)){
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(child_day_button), FALSE);
                day_idx_loop++;
            }
            child_day_button = gtk_widget_get_next_sibling(child_day_button);
        }
        gtk_drop_down_set_selected(GTK_DROP_DOWN(hour_dropdown_widget), 0);

        g_string_free(days_str_g, TRUE);
        g_object_unref(times_list_edit);
    }
}

static void on_edit_habits(GtkButton *button, AppData *app_data) {
    GtkWidget *edit_window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(edit_window), "Edit Habits");
    gtk_window_set_default_size(GTK_WINDOW(edit_window), 900, 600);
    gtk_window_set_transient_for(GTK_WINDOW(edit_window), GTK_WINDOW(app_data->main_window));
    gtk_window_set_modal(GTK_WINDOW(edit_window), TRUE);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(vbox, 10);
    gtk_widget_set_margin_end(vbox, 10);
    gtk_widget_set_margin_top(vbox, 10);
    gtk_widget_set_margin_bottom(vbox, 10);
    gtk_window_set_child(GTK_WINDOW(edit_window), vbox);

    GtkWidget *habits_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(habits_grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(habits_grid), 5);
    gtk_grid_set_column_homogeneous(GTK_GRID(habits_grid), TRUE);


    const char *labels[] = {"Habit", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun", "Time Slot", "Action"};
    for (int i = 0; i < 10; i++) {
        GtkWidget *label = gtk_label_new(labels[i]);
        gtk_widget_set_halign(label, GTK_ALIGN_CENTER);
        gtk_grid_attach(GTK_GRID(habits_grid), label, i, 0, 1, 1);
    }

    int current_row_idx = 1;
    const char *day_names[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    const char *times[] = {
        "00:00-02:00", "02:00-04:00", "04:00-06:00", "06:00-08:00",
        "08:00-10:00", "10:00-12:00", "12:00-14:00", "14:00-16:00",
        "16:00-18:00", "18:00-20:00", "20:00-22:00", "22:00-24:00"
    };

    for (GList *iter = app_data->habits; iter; iter = iter->next) {
        const char *habit_name = (const char *)iter->data;
        GtkWidget *row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);

        GtkWidget *label = gtk_label_new(habit_name);
         gtk_widget_set_hexpand(label, TRUE);
        gtk_widget_set_halign(label, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(row_box), label);


        char query[256];
        snprintf(query, sizeof(query), "SELECT days, time_slot FROM habit_tracking WHERE habit_name = '%s' AND (date = '' OR date IS NULL);", habit_name);
        sqlite3_stmt *stmt;
        const char *days_str = "";
        const char *time_slot_str = "";
        if (sqlite3_prepare_v2(app_data->db, query, -1, &stmt, NULL) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                const char *db_days = (const char *)sqlite3_column_text(stmt, 0);
                const char *db_time = (const char *)sqlite3_column_text(stmt, 1);
                if (db_days) days_str = db_days;
                if (db_time) time_slot_str = db_time;
            }
        }
        sqlite3_finalize(stmt);

        GtkWidget* day_buttons_in_row[7];

        for (int i = 0; i < 7; i++) {
            GtkWidget *day_button = gtk_toggle_button_new_with_label(day_names[i]);
            if (strstr(days_str, day_names[i])) {
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(day_button), TRUE);
            }
            g_object_set_data(G_OBJECT(day_button), "app_data", app_data);
            g_object_set_data(G_OBJECT(day_button), "habit_name", g_strdup(habit_name));
            day_buttons_in_row[i] = day_button;
            gtk_box_append(GTK_BOX(row_box), day_button);
        }

        GtkStringList *times_list = gtk_string_list_new(NULL);
        int selected_time = 0;
        for (int i = 0; i < 12; i++) {
            gtk_string_list_append(times_list, times[i]);
            if (strcmp(time_slot_str, times[i]) == 0) {
                selected_time = i;
            }
        }
        GtkWidget *hour_dropdown = gtk_drop_down_new(G_LIST_MODEL(times_list), NULL);
        gtk_drop_down_set_selected(GTK_DROP_DOWN(hour_dropdown), selected_time);
        g_object_set_data(G_OBJECT(hour_dropdown), "app_data", app_data);
        g_object_set_data(G_OBJECT(hour_dropdown), "habit_name", g_strdup(habit_name));

        for(int i=0; i<7; ++i){
            g_object_set_data(G_OBJECT(day_buttons_in_row[i]), "hour_dropdown", hour_dropdown);
            g_signal_connect(day_buttons_in_row[i], "toggled", G_CALLBACK(on_day_toggled), app_data);
        }
        g_signal_connect(hour_dropdown, "notify::selected", G_CALLBACK(on_day_toggled), app_data);

        gtk_box_append(GTK_BOX(row_box), hour_dropdown);

        GtkWidget *remove_button = gtk_button_new_with_label("Remove");
        g_object_set_data(G_OBJECT(remove_button), "habit_name", g_strdup(habit_name));
        g_object_set_data(G_OBJECT(remove_button), "row", row_box);
        g_signal_connect(remove_button, "clicked", G_CALLBACK(on_remove_habit), app_data);
        gtk_box_append(GTK_BOX(row_box), remove_button);

        gtk_grid_attach(GTK_GRID(habits_grid), row_box, 0, current_row_idx++, 10, 1);
        g_object_unref(times_list);
    }


    GtkWidget *scrolled_window = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scrolled_window, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled_window), habits_grid);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_box_append(GTK_BOX(vbox), scrolled_window);

    GtkWidget *separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_append(GTK_BOX(vbox), separator);

    GtkWidget *add_box_outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_box_append(GTK_BOX(vbox), add_box_outer);

    GtkWidget *add_habit_heading = gtk_label_new("Add New Habit");
    gtk_widget_add_css_class(add_habit_heading, "heading");
    gtk_widget_set_halign(add_habit_heading, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(add_box_outer), add_habit_heading);

    GtkWidget *add_box_fields = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_append(GTK_BOX(add_box_outer), add_box_fields);


    GtkWidget *habit_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(habit_entry), "Enter new habit name...");
    gtk_widget_set_hexpand(habit_entry, TRUE);
    gtk_box_append(GTK_BOX(add_box_fields), habit_entry);

    GtkWidget *days_box_new = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    for (int i = 0; i < 7; i++) {
        GtkWidget *day_button = gtk_toggle_button_new_with_label(day_names[i]);
        gtk_box_append(GTK_BOX(days_box_new), day_button);
    }
    gtk_box_append(GTK_BOX(add_box_fields), days_box_new);

    GtkStringList *times_list_new = gtk_string_list_new(NULL);
    for (int i = 0; i < 12; i++) {
        gtk_string_list_append(times_list_new, times[i]);
    }
    GtkWidget *hour_dropdown_new = gtk_drop_down_new(G_LIST_MODEL(times_list_new), NULL);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(hour_dropdown_new), 0);
    gtk_box_append(GTK_BOX(add_box_fields), hour_dropdown_new);

    GtkWidget *add_button_bottom = gtk_button_new_with_label("Add Habit");
    g_object_set_data(G_OBJECT(add_button_bottom), "app_data", app_data);
    g_object_set_data(G_OBJECT(add_button_bottom), "entry", habit_entry);
    g_object_set_data(G_OBJECT(add_button_bottom), "habits_grid", habits_grid);
    g_object_set_data(G_OBJECT(add_button_bottom), "days_box", days_box_new);
    g_object_set_data(G_OBJECT(add_button_bottom), "hour_dropdown", hour_dropdown_new);
    g_signal_connect(add_button_bottom, "clicked", G_CALLBACK(on_add_habit_in_edit), app_data);
    gtk_box_append(GTK_BOX(add_box_outer), add_button_bottom);
    gtk_widget_set_halign(add_button_bottom, GTK_ALIGN_CENTER);


    GtkWidget *separator2 = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_append(GTK_BOX(vbox), separator2);

    GtkWidget *save_button = gtk_button_new_with_label("Close");
    gtk_widget_set_halign(save_button, GTK_ALIGN_END);
    g_signal_connect_swapped(save_button, "clicked", G_CALLBACK(gtk_window_destroy), edit_window);
    gtk_box_append(GTK_BOX(vbox), save_button);

    gtk_window_present(GTK_WINDOW(edit_window));
    g_object_unref(times_list_new);
}


static void on_mark_task_done(GtkButton *button, AppData *app_data) {
    const char *task = (const char *)g_object_get_data(G_OBJECT(button), "task");
    const char *day = (const char *)g_object_get_data(G_OBJECT(button), "day");
    const char *time_slot = (const char *)g_object_get_data(G_OBJECT(button), "time_slot");
    GtkWidget *task_row = (GtkWidget *)g_object_get_data(G_OBJECT(button), "task_row");
    GtkWidget *task_label_timetable = (GtkWidget *)g_object_get_data(G_OBJECT(button), "task_label_timetable");


    if (task_label_timetable && gtk_widget_get_parent(task_label_timetable)) {
        gtk_box_remove(GTK_BOX(gtk_widget_get_parent(task_label_timetable)), task_label_timetable);
    }


    if (task_row && gtk_widget_get_parent(task_row)){
        gtk_box_remove(GTK_BOX(app_data->pending_tasks_box), task_row);
    }


    char insert_query[512];
    snprintf(insert_query, sizeof(insert_query),
             "INSERT INTO completed_tasks (task, day, time_slot) VALUES ('%s', '%s', '%s');",
             task, day, time_slot);
    if (sqlite3_exec(app_data->db, insert_query, NULL, NULL, NULL) != SQLITE_OK) {
        g_printerr("Failed to insert task into completed_tasks: %s\n", sqlite3_errmsg(app_data->db));
    }

    char delete_query[512];
    snprintf(delete_query, sizeof(delete_query),
             "DELETE FROM timetable_tasks WHERE task = '%s' AND day = '%s' AND time_slot = '%s';",
             task, day, time_slot);
    if (sqlite3_exec(app_data->db, delete_query, NULL, NULL, NULL) != SQLITE_OK) {
         g_printerr("Failed to delete task from timetable_tasks: %s\n", sqlite3_errmsg(app_data->db));
    }

    char task_display[512];
    snprintf(task_display, sizeof(task_display), "%s (%s, %s)", task, day, time_slot);
    GtkWidget *completed_task_label = gtk_label_new(task_display);
    gtk_widget_set_halign(completed_task_label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(app_data->completed_tasks_box), completed_task_label);

    g_free((char *)task);
    g_free((char *)day);
    g_free((char *)time_slot);
}

typedef struct {
    AppData *app_data;
    const char *day;
    const char *time_slot;
    GtkWidget *task_box_in_grid;
    GtkWidget *dialog;
    GtkWidget *entry;
} TaskDialogData;


static void on_add_task_ok_clicked(GtkButton *button, gpointer data) {
    TaskDialogData *task_data = data;

    const char *task_text = gtk_editable_get_text(GTK_EDITABLE(task_data->entry));
    if (task_text && strlen(task_text) > 0) {
        char query[512];
        snprintf(query, sizeof(query),
                 "INSERT INTO timetable_tasks (day, time_slot, task) VALUES ('%s', '%s', '%s');",
                 task_data->day, task_data->time_slot, task_text);
        if (sqlite3_exec(task_data->app_data->db, query, NULL, NULL, NULL) == SQLITE_OK) {

            GtkWidget *task_label_timetable = gtk_label_new(task_text);
            gtk_widget_set_halign(task_label_timetable, GTK_ALIGN_START);
            gtk_box_append(GTK_BOX(task_data->task_box_in_grid), task_label_timetable);

            char task_display[512];
            snprintf(task_display, sizeof(task_display), "%s (%s, %s)", task_text, task_data->day, task_data->time_slot);
            GtkWidget *task_row_pending = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
            GtkWidget *pending_task_label_widget = gtk_label_new(task_display);
            gtk_widget_set_halign(pending_task_label_widget, GTK_ALIGN_START);
            gtk_box_append(GTK_BOX(task_row_pending), pending_task_label_widget);

            GtkWidget *done_button = gtk_button_new_with_label("Mark as Done");
            g_object_set_data(G_OBJECT(done_button), "task", g_strdup(task_text));
            g_object_set_data(G_OBJECT(done_button), "day", g_strdup(task_data->day));
            g_object_set_data(G_OBJECT(done_button), "time_slot", g_strdup(task_data->time_slot));
            g_object_set_data(G_OBJECT(done_button), "task_row", task_row_pending);
            g_object_set_data(G_OBJECT(done_button), "task_label_timetable", task_label_timetable);
            g_signal_connect(done_button, "clicked", G_CALLBACK(on_mark_task_done), task_data->app_data);
            gtk_box_append(GTK_BOX(task_row_pending), done_button);

            gtk_box_append(GTK_BOX(task_data->app_data->pending_tasks_box), task_row_pending);
        } else {
            g_printerr("Failed to add task to database: %s\n", sqlite3_errmsg(task_data->app_data->db));
        }
    }

    gtk_window_destroy(GTK_WINDOW(task_data->dialog));
    g_free(task_data);
}

static void on_add_task_cancel_clicked(GtkButton *button, gpointer data) {
    TaskDialogData *task_data = data;
    gtk_window_destroy(GTK_WINDOW(task_data->dialog));
    g_free(task_data);
}

typedef struct {
    AppData *app_data;
    char *day;
    char *time_slot;
    GtkWidget *task_box_in_grid;
} TimetableCellData;


static void on_timetable_cell_clicked(GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y, gpointer data) {
    TimetableCellData *cell_data = data;

    GtkWidget *dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog), "Add Task");
    gtk_window_set_default_size(GTK_WINDOW(dialog), 350, 200);
    gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(cell_data->app_data->main_window));
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);


    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(vbox, 15);
    gtk_widget_set_margin_end(vbox, 15);
    gtk_widget_set_margin_top(vbox, 15);
    gtk_widget_set_margin_bottom(vbox, 15);
    gtk_window_set_child(GTK_WINDOW(dialog), vbox);

    char label_text[128];
    snprintf(label_text, sizeof(label_text), "Add task for %s at %s", cell_data->day, cell_data->time_slot);
    GtkWidget *label = gtk_label_new(label_text);
    gtk_widget_add_css_class(label,"heading");
    gtk_box_append(GTK_BOX(vbox), label);

    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Enter task description...");
    gtk_box_append(GTK_BOX(vbox), entry);

    GtkWidget *button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_halign(button_box, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(vbox), button_box);

    TaskDialogData *task_dialog_data = g_new0(TaskDialogData, 1);
    task_dialog_data->app_data = cell_data->app_data;
    task_dialog_data->day = cell_data->day;
    task_dialog_data->time_slot = cell_data->time_slot;
    task_dialog_data->task_box_in_grid = cell_data->task_box_in_grid;
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

static void free_timetable_cell_data(gpointer data) {
    TimetableCellData *cell_data = data;
    g_free(cell_data->day);
    g_free(cell_data->time_slot);
    g_free(cell_data);
}


static void on_remove_task(GtkButton *button, AppData *app_data) {
    const char *task = (const char *)g_object_get_data(G_OBJECT(button), "task");
    const char *day = (const char *)g_object_get_data(G_OBJECT(button), "day");
    const char *time_slot = (const char *)g_object_get_data(G_OBJECT(button), "time_slot");
    GtkWidget *row_widget = (GtkWidget *)g_object_get_data(G_OBJECT(button), "row");

    char query[512];
    snprintf(query, sizeof(query),
             "DELETE FROM timetable_tasks WHERE task = '%s' AND day = '%s' AND time_slot = '%s';",
             task, day, time_slot);
    if (sqlite3_exec(app_data->db, query, NULL, NULL, NULL) != SQLITE_OK) {
        g_printerr("Failed to delete task from timetable_tasks: %s\n", sqlite3_errmsg(app_data->db));
    }


    GtkWidget *child = gtk_widget_get_first_child(app_data->pending_tasks_box);
    while (child) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        GtkWidget *label_widget = gtk_widget_get_first_child(child);
        if (label_widget && GTK_IS_LABEL(label_widget)) {
            const char *label_text_pending = gtk_label_get_text(GTK_LABEL(label_widget));
            char task_display_pending[512];
            snprintf(task_display_pending, sizeof(task_display_pending), "%s (%s, %s)", task, day, time_slot);
            if (strcmp(label_text_pending, task_display_pending) == 0) {
                gtk_box_remove(GTK_BOX(app_data->pending_tasks_box), child);
                break;
            }
        }
        child = next;
    }

    const char *days_map[] = {"", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    const char *times_map[] = {
        "00:00-02:00", "02:00-04:00", "04:00-06:00", "06:00-08:00",
        "08:00-10:00", "10:00-12:00", "12:00-14:00", "14:00-16:00",
        "16:00-18:00", "18:00-20:00", "20:00-22:00", "22:00-24:00"
    };
    int day_idx_map = 0, time_idx_map = 0;
    for (int i = 1; i <= 7; i++) {
        if (strcmp(days_map[i], day) == 0) {
            day_idx_map = i;
            break;
        }
    }
    for (int i = 0; i < 12; i++) {
        if (strcmp(times_map[i], time_slot) == 0) {
            time_idx_map = i + 1;
            break;
        }
    }
    if (app_data->timetable_grid && day_idx_map > 0 && time_idx_map > 0) {
        GtkWidget *task_box_grid = gtk_grid_get_child_at(GTK_GRID(app_data->timetable_grid), day_idx_map, time_idx_map);
        if (task_box_grid) {
            GtkWidget *grid_child_label = gtk_widget_get_first_child(task_box_grid);
            while (grid_child_label) {
                 GtkWidget *next_grid_child = gtk_widget_get_next_sibling(grid_child_label);
                if (GTK_IS_LABEL(grid_child_label)){
                    const char *label_text_grid = gtk_label_get_text(GTK_LABEL(grid_child_label));
                    if (strcmp(label_text_grid, task) == 0) {
                        gtk_box_remove(GTK_BOX(task_box_grid), grid_child_label);
                        break;
                    }
                }
                grid_child_label = next_grid_child;
            }
        }
    }

    if (row_widget && gtk_widget_get_parent(row_widget)) {
        gtk_grid_remove(GTK_GRID(gtk_widget_get_parent(row_widget)), row_widget);
    }


    g_free((char *)task);
    g_free((char *)day);
    g_free((char *)time_slot);
}


static void on_add_task_in_edit(GtkButton *button, gpointer data) {
    AppData *app_data = (AppData *)g_object_get_data(G_OBJECT(button), "app_data");
    GtkWidget *entry_widget = (GtkWidget *)g_object_get_data(G_OBJECT(button), "entry");
    GtkWidget *calendar_widget = (GtkWidget *)g_object_get_data(G_OBJECT(button), "calendar");
    GtkWidget *hour_dropdown_widget = (GtkWidget *)g_object_get_data(G_OBJECT(button), "hour_dropdown");
    GtkWidget *tasks_grid_widget = (GtkWidget *)g_object_get_data(G_OBJECT(button), "tasks_grid");


    const char *task_text = gtk_editable_get_text(GTK_EDITABLE(entry_widget));
    if (task_text && strlen(task_text) > 0) {
        GDateTime *calendar_date = gtk_calendar_get_date(GTK_CALENDAR(calendar_widget));
        if (!calendar_date) return;

        guint year_val = g_date_time_get_year(calendar_date);
        guint month_val = g_date_time_get_month(calendar_date);
        guint day_val = g_date_time_get_day_of_month(calendar_date);
        g_date_time_unref(calendar_date);

        guint selected_hour_val = gtk_drop_down_get_selected(GTK_DROP_DOWN(hour_dropdown_widget));
        int hour_int = (int)selected_hour_val;

        GDateTime *due_date_time = g_date_time_new_local(year_val, month_val, day_val, hour_int, 0, 0);
        if (!due_date_time) return;


        const char *days_of_week_map[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"}; //GTK is 1-7 Mon-Sun
        int day_of_week_idx = g_date_time_get_day_of_week(due_date_time); // 1=Mon, ..., 7=Sun
        const char *day_str_val = days_of_week_map[day_of_week_idx -1];


        const char *time_slots_map[] = {
            "00:00-02:00", "02:00-04:00", "04:00-06:00", "06:00-08:00",
            "08:00-10:00", "10:00-12:00", "12:00-14:00", "14:00-16:00",
            "16:00-18:00", "18:00-20:00", "20:00-22:00", "22:00-24:00"
        };
        int time_slot_idx_val = hour_int / 2;
        const char *time_slot_str_val = time_slots_map[time_slot_idx_val];

        char query_insert[512];
        snprintf(query_insert, sizeof(query_insert),
                 "INSERT INTO timetable_tasks (day, time_slot, task) VALUES ('%s', '%s', '%s');",
                 day_str_val, time_slot_str_val, task_text);
        if (sqlite3_exec(app_data->db, query_insert, NULL, NULL, NULL) == SQLITE_OK) {

            int day_idx_grid = 0, time_idx_grid = 0;
             const char *days_grid_map[] = {"", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"}; // Grid is 1-indexed for days
            for (int i = 1; i <= 7; i++) {
                if (strcmp(days_grid_map[i], day_str_val) == 0) {
                    day_idx_grid = i;
                    break;
                }
            }
            for (int i = 0; i < 12; i++) {
                if (strcmp(time_slots_map[i], time_slot_str_val) == 0) {
                    time_idx_grid = i + 1; // Grid is 1-indexed for time slots
                    break;
                }
            }

            GtkWidget *task_label_timetable = NULL;
            if (app_data->timetable_grid && day_idx_grid > 0 && time_idx_grid > 0) {
                GtkWidget *task_box_cell = gtk_grid_get_child_at(GTK_GRID(app_data->timetable_grid), day_idx_grid, time_idx_grid);
                if (task_box_cell) {
                    task_label_timetable = gtk_label_new(task_text);
                    gtk_widget_set_halign(task_label_timetable, GTK_ALIGN_START);
                    gtk_box_append(GTK_BOX(task_box_cell), task_label_timetable);
                }
            }

            char task_display_pending[512];
            snprintf(task_display_pending, sizeof(task_display_pending), "%s (%s, %s)", task_text, day_str_val, time_slot_str_val);
            GtkWidget *task_row_box_pending = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
            GtkWidget *pending_task_label_ui = gtk_label_new(task_display_pending);
            gtk_widget_set_halign(pending_task_label_ui, GTK_ALIGN_START);
            gtk_box_append(GTK_BOX(task_row_box_pending), pending_task_label_ui);

            GtkWidget *done_button_pending = gtk_button_new_with_label("Mark as Done");
            g_object_set_data(G_OBJECT(done_button_pending), "task", g_strdup(task_text));
            g_object_set_data(G_OBJECT(done_button_pending), "day", g_strdup(day_str_val));
            g_object_set_data(G_OBJECT(done_button_pending), "time_slot", g_strdup(time_slot_str_val));
            g_object_set_data(G_OBJECT(done_button_pending), "task_row", task_row_box_pending);
            if (task_label_timetable) {
                g_object_set_data(G_OBJECT(done_button_pending), "task_label_timetable", task_label_timetable);
            }
            g_signal_connect(done_button_pending, "clicked", G_CALLBACK(on_mark_task_done), app_data);
            gtk_box_append(GTK_BOX(task_row_box_pending), done_button_pending);

            gtk_box_append(GTK_BOX(app_data->pending_tasks_box), task_row_box_pending);


            int next_row_edit_grid = 0;
            GtkWidget *edit_grid_child = gtk_widget_get_first_child(tasks_grid_widget);
            while(edit_grid_child){
                next_row_edit_grid++;
                edit_grid_child = gtk_widget_get_next_sibling(edit_grid_child);
            }
             if(next_row_edit_grid == 0) next_row_edit_grid = 1;


            GtkWidget *row_box_edit_task = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
            char display_edit_task[512];
            snprintf(display_edit_task, sizeof(display_edit_task), "%s (%s, %s)", task_text, day_str_val, time_slot_str_val);
            GtkWidget *task_label_edit_ui = gtk_label_new(display_edit_task);
             gtk_widget_set_hexpand(task_label_edit_ui, TRUE);
            gtk_widget_set_halign(task_label_edit_ui, GTK_ALIGN_START);
            gtk_box_append(GTK_BOX(row_box_edit_task), task_label_edit_ui);

            GtkWidget *remove_button_edit_task = gtk_button_new_with_label("Remove");
            g_object_set_data(G_OBJECT(remove_button_edit_task), "task", g_strdup(task_text));
            g_object_set_data(G_OBJECT(remove_button_edit_task), "day", g_strdup(day_str_val));
            g_object_set_data(G_OBJECT(remove_button_edit_task), "time_slot", g_strdup(time_slot_str_val));
            g_object_set_data(G_OBJECT(remove_button_edit_task), "row", row_box_edit_task);
            g_signal_connect(remove_button_edit_task, "clicked", G_CALLBACK(on_remove_task), app_data);
            gtk_box_append(GTK_BOX(row_box_edit_task), remove_button_edit_task);

            gtk_grid_attach(GTK_GRID(tasks_grid_widget), row_box_edit_task, 0, next_row_edit_grid, 2, 1);
        } else {
            g_printerr("Failed to add task to database: %s\n", sqlite3_errmsg(app_data->db));
        }

        g_date_time_unref(due_date_time);
        gtk_editable_set_text(GTK_EDITABLE(entry_widget), "");
        gtk_drop_down_set_selected(GTK_DROP_DOWN(hour_dropdown_widget), 0);
         gtk_calendar_select_day(GTK_CALENDAR(calendar_widget), g_date_time_new_now_local());

    }
}


static void on_edit_tasks(GtkButton *button, AppData *app_data) {
    GtkWidget *edit_window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(edit_window), "Edit Tasks");
    gtk_window_set_transient_for(GTK_WINDOW(edit_window), GTK_WINDOW(app_data->main_window));
    gtk_window_set_modal(GTK_WINDOW(edit_window), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(edit_window), 700, 700);


    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(vbox, 15);
    gtk_widget_set_margin_end(vbox, 15);
    gtk_widget_set_margin_top(vbox, 15);
    gtk_widget_set_margin_bottom(vbox, 15);
    gtk_window_set_child(GTK_WINDOW(edit_window), vbox);

    GtkWidget *tasks_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(tasks_grid), 5);
    gtk_grid_set_column_spacing(GTK_GRID(tasks_grid), 10);
    gtk_grid_set_column_homogeneous(GTK_GRID(tasks_grid), TRUE);


    const char *labels[] = {"Task (Day, Time Slot)", "Action"};
    GtkWidget* header_label1 = gtk_label_new(labels[0]);
    gtk_widget_set_halign(header_label1, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(tasks_grid), header_label1, 0, 0, 1, 1);
    GtkWidget* header_label2 = gtk_label_new(labels[1]);
    gtk_widget_set_halign(header_label2, GTK_ALIGN_CENTER);
    gtk_grid_attach(GTK_GRID(tasks_grid), header_label2, 1, 0, 1, 1);


    char query_select[256];
    snprintf(query_select, sizeof(query_select), "SELECT task, day, time_slot FROM timetable_tasks;");
    sqlite3_stmt *stmt_select;
    int row_idx = 1;
    if (sqlite3_prepare_v2(app_data->db, query_select, -1, &stmt_select, NULL) == SQLITE_OK) {
        while (sqlite3_step(stmt_select) == SQLITE_ROW) {
            const char *task_text = (const char *)sqlite3_column_text(stmt_select, 0);
            const char *day_text = (const char *)sqlite3_column_text(stmt_select, 1);
            const char *time_slot_text = (const char *)sqlite3_column_text(stmt_select, 2);

            GtkWidget *row_box_display = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
            char display_text[512];
            snprintf(display_text, sizeof(display_text), "%s (%s, %s)", task_text, day_text, time_slot_text);
            GtkWidget *task_label_display = gtk_label_new(display_text);
            gtk_widget_set_hexpand(task_label_display, TRUE);
            gtk_widget_set_halign(task_label_display, GTK_ALIGN_START);
            gtk_box_append(GTK_BOX(row_box_display), task_label_display);

            GtkWidget *remove_button_display = gtk_button_new_with_label("Remove");
            g_object_set_data(G_OBJECT(remove_button_display), "task", g_strdup(task_text));
            g_object_set_data(G_OBJECT(remove_button_display), "day", g_strdup(day_text));
            g_object_set_data(G_OBJECT(remove_button_display), "time_slot", g_strdup(time_slot_text));
            g_object_set_data(G_OBJECT(remove_button_display), "row", row_box_display);
            g_signal_connect(remove_button_display, "clicked", G_CALLBACK(on_remove_task), app_data);
            gtk_box_append(GTK_BOX(row_box_display), remove_button_display);

            gtk_grid_attach(GTK_GRID(tasks_grid), row_box_display, 0, row_idx++, 2, 1);
        }
    }
    sqlite3_finalize(stmt_select);

    GtkWidget *scrolled_window_tasks = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled_window_tasks), tasks_grid);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window_tasks),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scrolled_window_tasks, TRUE);
    gtk_box_append(GTK_BOX(vbox), scrolled_window_tasks);

    GtkWidget *separator_edit_task = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_append(GTK_BOX(vbox), separator_edit_task);

    GtkWidget *add_task_section_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_box_append(GTK_BOX(vbox), add_task_section_box);

    GtkWidget *add_task_heading = gtk_label_new("Add New Task");
    gtk_widget_add_css_class(add_task_heading, "heading");
    gtk_widget_set_halign(add_task_heading, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(add_task_section_box), add_task_heading);


    GtkWidget *task_entry_new = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(task_entry_new), "Enter new task description...");
    gtk_box_append(GTK_BOX(add_task_section_box), task_entry_new);

    GtkWidget *date_time_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_halign(date_time_box, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(add_task_section_box), date_time_box);

    GtkWidget *calendar_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_box_append(GTK_BOX(date_time_box), calendar_box);
    GtkWidget *calendar_label_new = gtk_label_new("Due Date:");
    gtk_widget_set_halign(calendar_label_new, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(calendar_box), calendar_label_new);
    GtkWidget *calendar_new = gtk_calendar_new();
    gtk_box_append(GTK_BOX(calendar_box), calendar_new);


    GtkWidget *time_select_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_widget_set_valign(time_select_box, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(date_time_box), time_select_box);
    GtkWidget *time_label_new = gtk_label_new("Due Time (Hour):");
    gtk_widget_set_halign(time_label_new, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(time_select_box), time_label_new);

    GtkStringList *hours_list_new = gtk_string_list_new(NULL);
    for (int i = 0; i < 24; i++) {
        char hour_str[3];
        snprintf(hour_str, sizeof(hour_str), "%02d", i);
        gtk_string_list_append(hours_list_new, hour_str);
    }
    GtkWidget *hour_dropdown_new_task = gtk_drop_down_new(G_LIST_MODEL(hours_list_new), NULL);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(hour_dropdown_new_task), 0);
    gtk_box_append(GTK_BOX(time_select_box), hour_dropdown_new_task);
    g_object_unref(hours_list_new);


    GtkWidget *add_button_new_task = gtk_button_new_with_label("Add Task");
    gtk_widget_set_halign(add_button_new_task, GTK_ALIGN_CENTER);
    g_object_set_data(G_OBJECT(add_button_new_task), "app_data", app_data);
    g_object_set_data(G_OBJECT(add_button_new_task), "entry", task_entry_new);
    g_object_set_data(G_OBJECT(add_button_new_task), "calendar", calendar_new);
    g_object_set_data(G_OBJECT(add_button_new_task), "hour_dropdown", hour_dropdown_new_task);
    g_object_set_data(G_OBJECT(add_button_new_task), "tasks_grid", tasks_grid);
    g_signal_connect(add_button_new_task, "clicked", G_CALLBACK(on_add_task_in_edit), app_data);
    gtk_box_append(GTK_BOX(add_task_section_box), add_button_new_task);

    GtkWidget *separator_edit_task2 = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_append(GTK_BOX(vbox), separator_edit_task2);

    GtkWidget *save_button_edit_task = gtk_button_new_with_label("Close");
    gtk_widget_set_halign(save_button_edit_task, GTK_ALIGN_END);
    g_signal_connect_swapped(save_button_edit_task, "clicked", G_CALLBACK(gtk_window_destroy), edit_window);
    gtk_box_append(GTK_BOX(vbox), save_button_edit_task);

    gtk_window_present(GTK_WINDOW(edit_window));
}


static GtkWidget *create_timetable_page(AppData *app_data) {
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 3);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 3);
    gtk_widget_set_margin_start(grid, 10);
    gtk_widget_set_margin_end(grid, 10);
    gtk_widget_set_margin_top(grid, 10);
    gtk_widget_set_margin_bottom(grid, 10);
    gtk_widget_set_hexpand(grid, TRUE);
    gtk_widget_set_vexpand(grid, TRUE);
    gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);
    gtk_grid_set_row_homogeneous(GTK_GRID(grid), TRUE);


    app_data->timetable_grid = grid;

    const char *days[] = {"", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    for (int i = 0; i < 8; i++) {
        GtkWidget *label = gtk_label_new(days[i]);
        gtk_widget_set_halign(label, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(label, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(label, "heading");
        gtk_grid_attach(GTK_GRID(grid), label, i, 0, 1, 1);
    }

    const char *times[] = {
        "00:00-02:00", "02:00-04:00", "04:00-06:00", "06:00-08:00",
        "08:00-10:00", "10:00-12:00", "12:00-14:00", "14:00-16:00",
        "16:00-18:00", "18:00-20:00", "20:00-22:00", "22:00-24:00"
    };
    for (int i = 0; i < 12; i++) {
        GtkWidget *label = gtk_label_new(times[i]);
        gtk_widget_set_halign(label, GTK_ALIGN_CENTER);
         gtk_widget_set_valign(label, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(label, "heading");
        gtk_grid_attach(GTK_GRID(grid), label, 0, i + 1, 1, 1);
    }

    for (int day_col = 1; day_col <= 7; day_col++) {
        for (int time_row = 1; time_row <= 12; time_row++) {
            GtkWidget *task_box_cell = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
            gtk_widget_set_hexpand(task_box_cell, TRUE);
            gtk_widget_set_vexpand(task_box_cell, TRUE);
            gtk_widget_add_css_class(task_box_cell, "task-slot");

            char query_tasks[256];
            snprintf(query_tasks, sizeof(query_tasks),
                     "SELECT task FROM timetable_tasks WHERE day = '%s' AND time_slot = '%s';",
                     days[day_col], times[time_row - 1]);
            sqlite3_stmt *stmt_tasks;
            if (sqlite3_prepare_v2(app_data->db, query_tasks, -1, &stmt_tasks, NULL) == SQLITE_OK) {
                while (sqlite3_step(stmt_tasks) == SQLITE_ROW) {
                    const char *task_text = (const char *)sqlite3_column_text(stmt_tasks, 0);
                    GtkWidget *task_label_ui = gtk_label_new(task_text);
                    gtk_widget_set_halign(task_label_ui, GTK_ALIGN_START);
                    gtk_widget_set_margin_start(task_label_ui, 5);
                    gtk_box_append(GTK_BOX(task_box_cell), task_label_ui);

                    char task_display_pending[512];
                    snprintf(task_display_pending, sizeof(task_display_pending), "%s (%s, %s)", task_text, days[day_col], times[time_row - 1]);
                    GtkWidget *task_row_pending_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
                    GtkWidget *pending_task_label_widget = gtk_label_new(task_display_pending);
                    gtk_widget_set_halign(pending_task_label_widget, GTK_ALIGN_START);
                     gtk_widget_set_hexpand(pending_task_label_widget, TRUE);
                    gtk_box_append(GTK_BOX(task_row_pending_box), pending_task_label_widget);

                    GtkWidget *done_button_pending = gtk_button_new_with_label("Mark as Done");
                    g_object_set_data(G_OBJECT(done_button_pending), "task", g_strdup(task_text));
                    g_object_set_data(G_OBJECT(done_button_pending), "day", g_strdup(days[day_col]));
                    g_object_set_data(G_OBJECT(done_button_pending), "time_slot", g_strdup(times[time_row - 1]));
                    g_object_set_data(G_OBJECT(done_button_pending), "task_row", task_row_pending_box);
                    g_object_set_data(G_OBJECT(done_button_pending), "task_label_timetable", task_label_ui);
                    g_signal_connect(done_button_pending, "clicked", G_CALLBACK(on_mark_task_done), app_data);
                    gtk_box_append(GTK_BOX(task_row_pending_box), done_button_pending);

                    gtk_box_append(GTK_BOX(app_data->pending_tasks_box), task_row_pending_box);
                }
            }
            sqlite3_finalize(stmt_tasks);

            char habit_query[256];
            snprintf(habit_query, sizeof(habit_query),
                     "SELECT habit_name, days FROM habit_tracking WHERE time_slot = '%s' AND (date = '' OR date IS NULL);", // only base habit defs
                     times[time_row - 1]);
            sqlite3_stmt *habit_stmt;
            if (sqlite3_prepare_v2(app_data->db, habit_query, -1, &habit_stmt, NULL) == SQLITE_OK) {
                while (sqlite3_step(habit_stmt) == SQLITE_ROW) {
                    const char *habit_name_text = (const char *)sqlite3_column_text(habit_stmt, 0);
                    const char *habit_days_text = (const char *)sqlite3_column_text(habit_stmt, 1);

                    if (habit_days_text && strstr(habit_days_text, days[day_col])) {
                        GtkWidget *habit_label_ui = gtk_label_new(habit_name_text);
                        gtk_widget_set_halign(habit_label_ui, GTK_ALIGN_START);
                        gtk_widget_add_css_class(habit_label_ui, "habit-label");
                        gtk_widget_set_margin_start(habit_label_ui, 5);
                        gtk_box_append(GTK_BOX(task_box_cell), habit_label_ui);
                    }
                }
            }
            sqlite3_finalize(habit_stmt);

            GtkGesture *click_controller = gtk_gesture_click_new();
            TimetableCellData *cell_data = g_new0(TimetableCellData, 1);
            cell_data->app_data = app_data;
            cell_data->day = g_strdup(days[day_col]);
            cell_data->time_slot = g_strdup(times[time_row - 1]);
            cell_data->task_box_in_grid = task_box_cell;

            g_signal_connect_data(click_controller, "pressed", G_CALLBACK(on_timetable_cell_clicked), cell_data, (GClosureNotify)free_timetable_cell_data, 0);
            gtk_widget_add_controller(task_box_cell, GTK_EVENT_CONTROLLER(click_controller));

            gtk_grid_attach(GTK_GRID(grid), task_box_cell, day_col, time_row, 1, 1);
        }
    }

    char completed_query[256];
    snprintf(completed_query, sizeof(completed_query), "SELECT task, day, time_slot FROM completed_tasks;");
    sqlite3_stmt *completed_stmt;
    if (sqlite3_prepare_v2(app_data->db, completed_query, -1, &completed_stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(completed_stmt) == SQLITE_ROW) {
            const char *task_text = (const char *)sqlite3_column_text(completed_stmt, 0);
            const char *day_text = (const char *)sqlite3_column_text(completed_stmt, 1);
            const char *time_slot_text = (const char *)sqlite3_column_text(completed_stmt, 2);

            char task_display_completed[512];
            snprintf(task_display_completed, sizeof(task_display_completed), "%s (%s, %s)", task_text, day_text, time_slot_text);
            GtkWidget *completed_task_label_ui = gtk_label_new(task_display_completed);
            gtk_widget_set_halign(completed_task_label_ui, GTK_ALIGN_START);
            gtk_box_append(GTK_BOX(app_data->completed_tasks_box), completed_task_label_ui);
        }
    }
    sqlite3_finalize(completed_stmt);

    return grid;
}

static void cleanup_app_data(AppData *app_data) {
    g_list_free_full(app_data->habits, g_free);
    app_data->habits = NULL;

    for (GList *iter = app_data->habit_widgets; iter; iter = iter->next) {
        GtkWidget *habit_box_widget = (GtkWidget *)iter->data;
        char *habit_name_data = (char *)g_object_get_data(G_OBJECT(habit_box_widget), "habit_name");
        if (habit_name_data) g_free(habit_name_data);

        GtkWidget *child_iter = gtk_widget_get_first_child(habit_box_widget);
        while (child_iter) {
            if (GTK_IS_DRAWING_AREA(child_iter)){
                 char *drawing_habit_name = (char *)g_object_get_data(G_OBJECT(child_iter), "habit_name");
                 if(drawing_habit_name) g_free(drawing_habit_name);
            }
            if (GTK_IS_BUTTON(child_iter) && strcmp(gtk_button_get_label(GTK_BUTTON(child_iter)), "Done Today") == 0) {
                char *button_habit_name_data = (char *)g_object_get_data(G_OBJECT(child_iter), "habit_name");
                if (button_habit_name_data) g_free(button_habit_name_data);
            }
            child_iter = gtk_widget_get_next_sibling(child_iter);
        }
    }
    g_list_free(app_data->habit_widgets);
    app_data->habit_widgets = NULL;

    if (app_data->db) {
        sqlite3_close(app_data->db);
        app_data->db = NULL;
    }

    g_free(app_data);
}

static void on_done_today_clicked(GtkButton *button, AppData *app_data) {
    const char *habit_name = (const char *)g_object_get_data(G_OBJECT(button), "habit_name");
    GtkWidget *drawing_area = (GtkWidget *)g_object_get_data(G_OBJECT(button), "drawing_area");

    GDateTime *date_time = g_date_time_new_now_local();
    if (!date_time) return;

    gint year_val = g_date_time_get_year(date_time);
    gint month_val = g_date_time_get_month(date_time);
    gint day_val = g_date_time_get_day_of_month(date_time);
    char date_str_val[11];
    snprintf(date_str_val, sizeof(date_str_val), "%04d-%02d-%02d", year_val, month_val, day_val);

    g_date_time_unref(date_time);

    char query_select[256];
    snprintf(query_select, sizeof(query_select),
             "SELECT completed FROM habit_tracking WHERE habit_name = '%s' AND date = '%s';",
             habit_name, date_str_val);

    sqlite3_stmt *stmt_select;
    int completed_status = 0; // Default to not completed
    gboolean entry_exists = FALSE;

    if (sqlite3_prepare_v2(app_data->db, query_select, -1, &stmt_select, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt_select) == SQLITE_ROW) {
            completed_status = sqlite3_column_int(stmt_select, 0);
            entry_exists = TRUE;
        }
    }
    sqlite3_finalize(stmt_select);

    char query_update[512];
    if (entry_exists) {
         completed_status = !completed_status; // Toggle if exists
         snprintf(query_update, sizeof(query_update),
                 "UPDATE habit_tracking SET completed = %d WHERE habit_name = '%s' AND date = '%s';",
                 completed_status, habit_name, date_str_val);
    } else {
        char fetch_details_query[256];
        snprintf(fetch_details_query, sizeof(fetch_details_query),
                 "SELECT days, time_slot FROM habit_tracking WHERE habit_name = '%s' AND (date = '' OR date IS NULL);", habit_name);
        sqlite3_stmt *stmt_details;
        const char *days_str = "";
        const char *time_slot_str = "";
        if (sqlite3_prepare_v2(app_data->db, fetch_details_query, -1, &stmt_details, NULL) == SQLITE_OK) {
            if (sqlite3_step(stmt_details) == SQLITE_ROW) {
                const char* fetched_days = (const char*)sqlite3_column_text(stmt_details, 0);
                const char* fetched_time_slot = (const char*)sqlite3_column_text(stmt_details, 1);
                if(fetched_days) days_str = fetched_days;
                if(fetched_time_slot) time_slot_str = fetched_time_slot;
            }
        }
        sqlite3_finalize(stmt_details);

        completed_status = 1;
        snprintf(query_update, sizeof(query_update),
                 "INSERT INTO habit_tracking (date, habit_name, completed, days, time_slot) "
                 "VALUES ('%s', '%s', %d, '%s', '%s');",
                 date_str_val, habit_name, completed_status, days_str, time_slot_str);
    }


    if (sqlite3_exec(app_data->db, query_update, NULL, NULL, NULL) != SQLITE_OK) {
        g_printerr("Failed to update habit completion: %s\n", sqlite3_errmsg(app_data->db));
    }


    if (drawing_area) {
        gtk_widget_queue_draw(drawing_area);
    }
}


static void on_activate(GtkApplication *app, gpointer user_data) {
    GtkSettings *settings = gtk_settings_get_default();
    g_object_set(settings, "gtk-application-prefer-dark-theme", TRUE, NULL);

    GtkCssProvider *css_provider = gtk_css_provider_new();
    const char *css =
        "* { font-family: 'Segoe UI', Cantarell, 'Open Sans', 'Helvetica Neue', sans-serif; }"
        "window { background-color: #1E1E2A; }"
        "notebook { background-color: rgba(40, 40, 60, 0.7); border-radius: 5px; padding: 5px; }"
        "notebook header { background-color: transparent; }"
        "notebook tab { background-color: rgba(60, 60, 80, 0.7); padding: 12px 18px; font-size: 16px; color: #D0D0E0; border-radius: 5px 5px 0 0; margin-right: 2px; border: 1px solid transparent; border-bottom: none; }"
        "notebook tab:hover { background-color: rgba(70, 70, 90, 0.8); }"
        "notebook tab:checked { background-color: #3A3A5A; color: white; border: 1px solid #4A4A6A; border-bottom: none; }"
        "scrolledwindow { background-color: rgba(40, 40, 60, 0.5); border-radius: 3px; }"
        "label { font-size: 15px; color: #EAEAEA; }"
        ".heading { font-size: 22px; font-weight: bold; color: #FFFFFF; margin-top: 10px; margin-bottom: 15px; }"
        "button { font-size: 15px; padding: 10px 18px; border-radius: 8px; transition: all 0.15s ease-in-out; background-color: #505075; color: white; border: 1px solid #65658A; box-shadow: 0 2px 4px rgba(0,0,0,0.2); }"
        "button:hover { transform: scale(1.03) translateY(-2px); box-shadow: 0 4px 8px rgba(80, 80, 130, 0.7); background-color: #606085; border-color: #75759A; }"
        "button:active { transform: scale(0.99) translateY(-1px); background-color: #454565; }"
        "entry, textview { font-size: 14px; background-color: rgba(30, 30, 45, 0.85); color: #E0E0E0; border: 1px solid #5A5A7A; border-radius: 5px; padding: 8px; }"
        "entry:focus, textview:focus { border-color: #7070A0; box-shadow: 0 0 5px rgba(100, 100, 150, 0.5); }"
        ".task-slot { border: 1px solid #4A4A6A; background-color: rgba(50, 50, 70, 0.65); padding: 8px; border-radius: 4px; transition: background-color 0.2s ease-in-out; }"
        ".task-slot:hover { background-color: rgba(60, 60, 80, 0.75); }"
        ".habit-label { color: #90B0E0; font-weight: normal; font-size: 13px; }"
        "checkbutton label, radiobutton label { font-size: 14px; color: #E0E0E0; }"
        "togglebutton { padding: 8px 10px; font-size: 13px; background-color: #484868; border-radius: 5px; border: 1px solid #585878; color: #EAEAEA; }"
        "togglebutton:checked { background-color: #6A6AA0; border-color: #7A7AC0; color: white; }"
        "togglebutton:hover { background-color: #585878; }"
        "togglebutton:checked:hover { background-color: #7A7AC0; }"
        "calendar { background-color: rgba(40, 40, 55, 0.9); padding: 10px; border-radius: 5px; color: #EAEAEA; }"
        "calendar:selected { background-color: #505075; color: white; }"
        "calendar .header { font-size: 18px; font-weight: bold; padding-bottom: 10px; color: #EAEAEA; }"
        "calendar .day-name { font-size: 13px; color: #C0C0C0; }"
        "calendar .day-number { font-size: 14px; color: #D0D0D0; border-radius: 3px; }"
        "calendar .day-number:hover { background-color: rgba(80,80,110,0.5); }"
        "calendar .day-number.selected { background-color: #505075; color: white; }"
        "calendar .other-month { color: #707080; }"
        "dropdown, dropdown listview { background-color: rgba(30, 30, 45, 0.85); color: #E0E0E0; border: 1px solid #5A5A7A; border-radius: 5px; padding: 3px; }"
        "dropdown listview row:selected { background-color: #505075 !important; color: white !important; }"
        "dropdown listview row:hover { background-color: rgba(80,80,110,0.5); }"
        "grid label { font-weight: bold; color: #D5D5D5; font-size: 15px; }"
        "separator { background-color: #4A4A6A; min-height: 1px; margin-top: 10px; margin-bottom: 10px; }";

    gtk_css_provider_load_from_string(css_provider, css);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                               GTK_STYLE_PROVIDER(css_provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css_provider);


    AppData *app_data = g_new0(AppData, 1);
    app_data->habits = NULL;
    app_data->habit_widgets = NULL;
    app_data->timetable_grid = NULL;

    if (sqlite3_open("habit_tracker.db", &app_data->db) != SQLITE_OK) {
        g_printerr("Cannot open database: %s\n", sqlite3_errmsg(app_data->db));
        cleanup_app_data(app_data);
        return;
    }

    /*
    const char *drop_habits_table_sql = "DROP TABLE IF EXISTS habit_tracking;";
    sqlite3_exec(app_data->db, drop_habits_table_sql, NULL, NULL, NULL);
    const char *drop_tasks_table_sql = "DROP TABLE IF EXISTS timetable_tasks;";
    sqlite3_exec(app_data->db, drop_tasks_table_sql, NULL, NULL, NULL);
    const char *drop_completed_tasks_table_sql = "DROP TABLE IF EXISTS completed_tasks;";
    sqlite3_exec(app_data->db, drop_completed_tasks_table_sql, NULL, NULL, NULL);
    */

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

    const char *create_tasks_table_sql =
        "CREATE TABLE IF NOT EXISTS timetable_tasks ("
        "day TEXT, time_slot TEXT, task TEXT, "
        "PRIMARY KEY (day, time_slot, task));";
    if (sqlite3_exec(app_data->db, create_tasks_table_sql, NULL, NULL, NULL) != SQLITE_OK) {
        g_printerr("Failed to create timetable_tasks table: %s\n", sqlite3_errmsg(app_data->db));
        cleanup_app_data(app_data);
        return;
    }

    const char *create_completed_tasks_table_sql =
        "CREATE TABLE IF NOT EXISTS completed_tasks ("
        "task TEXT, day TEXT, time_slot TEXT, "
        "PRIMARY KEY (task, day, time_slot));";
    if (sqlite3_exec(app_data->db, create_completed_tasks_table_sql, NULL, NULL, NULL) != SQLITE_OK) {
        g_printerr("Failed to create completed_tasks table: %s\n", sqlite3_errmsg(app_data->db));
        cleanup_app_data(app_data);
        return;
    }

    app_data->main_window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(app_data->main_window), "SereneTrack Habit & Task Manager");
    gtk_window_set_default_size(GTK_WINDOW(app_data->main_window), 1000, 750);


    GtkWidget *notebook = gtk_notebook_new();
    gtk_window_set_child(GTK_WINDOW(app_data->main_window), notebook);

    GtkWidget *habits_page_scroll = gtk_scrolled_window_new();
    gtk_widget_set_name(habits_page_scroll, "habits-page-scroll");
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(habits_page_scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), habits_page_scroll, gtk_label_new("Dashboard & Habits"));


    GtkWidget *habits_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 20);
    gtk_widget_set_margin_start(habits_page, 20);
    gtk_widget_set_margin_end(habits_page, 20);
    gtk_widget_set_margin_top(habits_page, 20);
    gtk_widget_set_margin_bottom(habits_page, 20);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(habits_page_scroll), habits_page);


    GtkWidget *pending_tasks_label = gtk_label_new("Pending Tasks");
    gtk_widget_add_css_class(pending_tasks_label, "heading");
    gtk_widget_set_halign(pending_tasks_label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(habits_page), pending_tasks_label);

    app_data->pending_tasks_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
     gtk_widget_set_margin_bottom(app_data->pending_tasks_box, 15);
    gtk_box_append(GTK_BOX(habits_page), app_data->pending_tasks_box);

    GtkWidget *separator1 = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_append(GTK_BOX(habits_page), separator1);

    GtkWidget *completed_tasks_label = gtk_label_new("Completed Tasks");
    gtk_widget_add_css_class(completed_tasks_label, "heading");
    gtk_widget_set_halign(completed_tasks_label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(habits_page), completed_tasks_label);

    app_data->completed_tasks_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_bottom(app_data->completed_tasks_box, 15);
    gtk_box_append(GTK_BOX(habits_page), app_data->completed_tasks_box);

    GtkWidget *separator2 = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_append(GTK_BOX(habits_page), separator2);

    GtkWidget *habits_label = gtk_label_new("Your Habits");
    gtk_widget_add_css_class(habits_label, "heading");
    gtk_widget_set_halign(habits_label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(habits_page), habits_label);

    app_data->habits_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 15);
    gtk_widget_set_halign(app_data->habits_box, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_bottom(app_data->habits_box, 15);
    GtkWidget* habits_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(habits_scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(habits_scroll), app_data->habits_box);
    gtk_widget_set_vexpand(habits_scroll, FALSE);
    gtk_box_append(GTK_BOX(habits_page), habits_scroll);


    char query_load_habits[256];
    snprintf(query_load_habits, sizeof(query_load_habits), "SELECT DISTINCT habit_name, days FROM habit_tracking WHERE date = '' OR date IS NULL;");
    sqlite3_stmt *stmt_load_habits;
    if (sqlite3_prepare_v2(app_data->db, query_load_habits, -1, &stmt_load_habits, NULL) == SQLITE_OK) {
        GDateTime *today = g_date_time_new_now_local();
        int day_of_week = g_date_time_get_day_of_week(today);
        const char *day_names_map[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
        const char *current_day_name = day_names_map[day_of_week -1];
        g_date_time_unref(today);

        while (sqlite3_step(stmt_load_habits) == SQLITE_ROW) {
            const char *habit_name_db = (const char *)sqlite3_column_text(stmt_load_habits, 0);
            const char *days_str_db = (const char *)sqlite3_column_text(stmt_load_habits, 1);
            if (!days_str_db) days_str_db = "";

            app_data->habits = g_list_append(app_data->habits, g_strdup(habit_name_db));

            GtkWidget *habit_box_ui = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
            gtk_widget_set_valign(habit_box_ui, GTK_ALIGN_START);
            GtkWidget *drawing_area_ui = gtk_drawing_area_new();
            gtk_widget_set_size_request(drawing_area_ui, 70, 70);
            gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(drawing_area_ui), draw_habit_logo, g_strdup(habit_name_db), g_free);
            g_object_set_data(G_OBJECT(drawing_area_ui), "app_data", app_data);
            g_object_set_data(G_OBJECT(drawing_area_ui), "habit_name", g_strdup(habit_name_db));
            gtk_box_append(GTK_BOX(habit_box_ui), drawing_area_ui);

            g_object_set_data(G_OBJECT(habit_box_ui), "habit_name", g_strdup(habit_name_db));

            GtkWidget *label_ui = gtk_label_new(habit_name_db);
            gtk_widget_set_halign(label_ui, GTK_ALIGN_CENTER);
            gtk_box_append(GTK_BOX(habit_box_ui), label_ui);

            if (strstr(days_str_db, current_day_name)) {
                GtkWidget *done_button_ui = gtk_button_new_with_label("Done Today");
                g_object_set_data(G_OBJECT(done_button_ui), "habit_name", g_strdup(habit_name_db));
                g_object_set_data(G_OBJECT(done_button_ui), "drawing_area", drawing_area_ui);
                g_signal_connect(done_button_ui, "clicked", G_CALLBACK(on_done_today_clicked), app_data);
                gtk_box_append(GTK_BOX(habit_box_ui), done_button_ui);
            }

            gtk_box_append(GTK_BOX(app_data->habits_box), habit_box_ui);
            app_data->habit_widgets = g_list_append(app_data->habit_widgets, habit_box_ui);
        }
    }
    sqlite3_finalize(stmt_load_habits);

    GtkWidget* management_buttons_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_halign(management_buttons_box, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(habits_page), management_buttons_box);


    GtkWidget *edit_habits_button = gtk_button_new_with_label("Manage Habits");
    g_signal_connect(edit_habits_button, "clicked", G_CALLBACK(on_edit_habits), app_data);
    gtk_box_append(GTK_BOX(management_buttons_box), edit_habits_button);

    GtkWidget *edit_tasks_button = gtk_button_new_with_label("Manage Tasks");
    g_signal_connect(edit_tasks_button, "clicked", G_CALLBACK(on_edit_tasks), app_data);
    gtk_box_append(GTK_BOX(management_buttons_box), edit_tasks_button);


    GtkWidget *timetable_page_content = create_timetable_page(app_data);
    GtkWidget *timetable_scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(timetable_scrolled), timetable_page_content);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(timetable_scrolled),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), timetable_scrolled, gtk_label_new("Weekly Timetable"));


    g_signal_connect(app_data->main_window, "destroy", G_CALLBACK(cleanup_app_data), app_data);
    gtk_window_present(GTK_WINDOW(app_data->main_window));
}

int main(int argc, char *argv[]) {
    GtkApplication *app = gtk_application_new("org.example.serenetrack", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}