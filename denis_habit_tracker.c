#include <gtk/gtk.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <cairo.h>

// Structure to hold application data
typedef struct {
    GtkWidget *main_window;
    GtkWidget *habits_vbox;
    GList *habits;        // Dynamic list of habit names (char *)
    GList *habit_widgets; // Dynamic list of habit widgets (GtkWidget *)
    sqlite3 *db;
    GtkWidget *habits_box; // Container for habits
    GtkWidget *date_label; // Date label for header bar
} AppData;

// Function prototypes
static void on_habit_toggled(GtkCheckButton *check_button, AppData *app_data);
static int get_days_completed(sqlite3 *db, const char *habit_name);
static void draw_habit_logo(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data);
static void on_remove_habit(GtkButton *button, AppData *app_data);
static void on_add_habit_in_edit(GtkButton *button, gpointer data);
static void on_edit_habits(GtkButton *button, AppData *app_data);
static void cleanup_app_data(AppData *app_data);
static GtkWidget *create_timetable_page(void);
static void on_activate(GtkApplication *app, gpointer user_data);


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
    cairo_select_font_face(cr, "Roboto", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
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

    // Remove from habits list
    GList *link = g_list_find_custom(app_data->habits, habit_name, (GCompareFunc)strcmp);
    if (link) {
        g_free(link->data);
        app_data->habits = g_list_delete_link(app_data->habits, link);
    }

    // Remove from UI in edit window
    gtk_box_remove(GTK_BOX(gtk_widget_get_parent(row)), row);
    gtk_widget_unparent(row);

    // Remove from main page via habit_widgets list
    GList *iter = app_data->habit_widgets;
    while (iter) {
        GtkWidget *widget = (GtkWidget *)iter->data;
        const char *name = (const char *)g_object_get_data(G_OBJECT(widget), "habit_name");
        if (name && strcmp(name, habit_name) == 0) {
            gtk_box_remove(GTK_BOX(app_data->habits_box), widget);
            app_data->habit_widgets = g_list_remove(app_data->habit_widgets, widget);
            g_free((char *)name);
            gtk_widget_unparent(widget);
            break;
        }
        iter = iter->next;
    }

    // Remove from database
    char query[256];
    snprintf(query, sizeof(query), "DELETE FROM habit_tracking WHERE habit_name = '%s';", habit_name);
    sqlite3_exec(app_data->db, query, NULL, NULL, NULL);
}

// Callback to add a habit in the edit window
static void on_add_habit_in_edit(GtkButton *button, gpointer data) {
    AppData *app_data = (AppData *)g_object_get_data(G_OBJECT(button), "app_data");
    GtkWidget *entry = (GtkWidget *)g_object_get_data(G_OBJECT(button), "entry");
    GtkWidget *habits_list = (GtkWidget *)g_object_get_data(G_OBJECT(button), "habits_list");

    const char *habit_name = gtk_editable_get_text(GTK_EDITABLE(entry));
    if (habit_name && strlen(habit_name) > 0) {
        app_data->habits = g_list_append(app_data->habits, g_strdup(habit_name));

        // Add to main page
        GtkWidget *habit_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
        gtk_widget_add_css_class(habit_box, "card");
        GtkWidget *drawing_area = gtk_drawing_area_new();
        gtk_widget_set_size_request(drawing_area, 60, 60);
        gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(drawing_area), draw_habit_logo, g_strdup(habit_name), g_free);
        g_object_set_data(G_OBJECT(drawing_area), "app_data", app_data);
        g_object_set_data(G_OBJECT(drawing_area), "habit_name", g_strdup(habit_name));
        g_object_set_data(G_OBJECT(habit_box), "habit_name", g_strdup(habit_name));

        gtk_box_append(GTK_BOX(habit_box), drawing_area);

        GtkWidget *label = gtk_label_new(habit_name);
        gtk_widget_set_halign(label, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(label, "review-option-text");
        gtk_box_append(GTK_BOX(habit_box), label);

        gtk_box_append(GTK_BOX(app_data->habits_box), habit_box);
        app_data->habit_widgets = g_list_append(app_data->habit_widgets, habit_box);

        // Add to edit window
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
        gtk_widget_add_css_class(row, "review-item");
        GtkWidget *row_label = gtk_label_new(habit_name);
        gtk_widget_add_css_class(row_label, "review-option-text");
        gtk_box_append(GTK_BOX(row), row_label);

        GtkWidget *remove_button = gtk_button_new_with_label("Remove");
        gtk_widget_add_css_class(remove_button, "destructive-action");
        g_object_set_data(G_OBJECT(remove_button), "habit_name", g_strdup(habit_name));
        g_object_set_data(G_OBJECT(remove_button), "row", row);
        g_signal_connect(remove_button, "clicked", G_CALLBACK(on_remove_habit), app_data);
        gtk_box_append(GTK_BOX(row), remove_button);

        gtk_box_append(GTK_BOX(habits_list), row);

        // Clear entry
        gtk_editable_set_text(GTK_EDITABLE(entry), "");
    }
}

// Callback to open the edit habits window
static void on_edit_habits(GtkButton *button, AppData *app_data) {
    GtkWidget *edit_window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(edit_window), "Edit Habits");
    gtk_window_set_default_size(GTK_WINDOW(edit_window), 450, 350);
    gtk_window_set_transient_for(GTK_WINDOW(edit_window), GTK_WINDOW(app_data->main_window));
    gtk_window_set_modal(GTK_WINDOW(edit_window), TRUE);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 20);
    gtk_widget_set_halign(vbox, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(vbox, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(vbox, "card");
    gtk_window_set_child(GTK_WINDOW(edit_window), vbox);

    GtkWidget *title = gtk_label_new("Manage Habits");
    gtk_widget_add_css_class(title, "title-1");
    gtk_box_append(GTK_BOX(vbox), title);

    GtkWidget *frame = gtk_frame_new(NULL);
    gtk_widget_add_css_class(frame, "form-frame");
    GtkWidget *inner_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 15);
    gtk_frame_set_child(GTK_FRAME(frame), inner_box);
    gtk_box_append(GTK_BOX(vbox), frame);

    // List of habits with remove buttons
    GtkWidget *habits_list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    for (GList *iter = app_data->habits; iter; iter = iter->next) {
        const char *habit_name = (const char *)iter->data;

        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
        gtk_widget_add_css_class(row, "review-item");
        GtkWidget *label = gtk_label_new(habit_name);
        gtk_widget_add_css_class(label, "review-option-text");
        gtk_box_append(GTK_BOX(row), label);

        GtkWidget *remove_button = gtk_button_new_with_label("Remove");
        gtk_widget_add_css_class(remove_button, "destructive-action");
        g_object_set_data(G_OBJECT(remove_button), "habit_name", g_strdup(habit_name));
        g_object_set_data(G_OBJECT(remove_button), "row", row);
        g_signal_connect(remove_button, "clicked", G_CALLBACK(on_remove_habit), app_data);
        gtk_box_append(GTK_BOX(row), remove_button);

        gtk_box_append(GTK_BOX(habits_list), row);
    }
    gtk_box_append(GTK_BOX(inner_box), habits_list);

    // Add new habit
    GtkWidget *habit_entry_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *icon = gtk_image_new_from_icon_name("document-edit-symbolic");
    gtk_widget_add_css_class(icon, "entry-icon");
    GtkWidget *habit_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(habit_entry), "Enter new habit...");
    gtk_widget_add_css_class(habit_entry, "entry");
    gtk_box_append(GTK_BOX(habit_entry_box), icon);
    gtk_box_append(GTK_BOX(habit_entry_box), habit_entry);
    gtk_box_append(GTK_BOX(inner_box), habit_entry_box);

    GtkWidget *add_button = gtk_button_new_with_label("Add Habit");
    gtk_widget_add_css_class(add_button, "gradient-button");
    g_object_set_data(G_OBJECT(add_button), "app_data", app_data);
    g_object_set_data(G_OBJECT(add_button), "entry", habit_entry);
    g_object_set_data(G_OBJECT(add_button), "habits_list", habits_list);
    g_signal_connect(add_button, "clicked", G_CALLBACK(on_add_habit_in_edit), app_data);
    gtk_box_append(GTK_BOX(inner_box), add_button);

    gtk_window_present(GTK_WINDOW(edit_window));
}

// Cleanup function for AppData
static void cleanup_app_data(AppData *app_data) {
    g_list_free_full(app_data->habits, g_free);
    g_list_free(app_data->habit_widgets);
    if (app_data->db) {
        sqlite3_close(app_data->db);
    }
    g_free(app_data);
}

// Create the timetable page
static GtkWidget *create_timetable_page(void) {


    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 5);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 5);
    gtk_widget_set_margin_start(grid, 10);
    gtk_widget_set_margin_end(grid, 10);
    gtk_widget_set_margin_top(grid, 10);
    gtk_widget_set_margin_bottom(grid, 10);

    // Days of the week as column headers
    const char *days[] = {"", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    for (int i = 0; i < 8; i++) {
        GtkWidget *label = gtk_label_new(days[i]);
        gtk_widget_set_halign(label, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(label, "review-option-text");
        gtk_grid_attach(GTK_GRID(grid), label, i, 0, 1, 1);
    }

    // Time intervals as row headers
    const char *times[] = {
        "00:00-02:00", "02:00-04:00", "04:00-06:00", "06:00-08:00",
        "08:00-10:00", "10:00-12:00", "12:00-14:00", "14:00-16:00",
        "16:00-18:00", "18:00-20:00", "20:00-22:00", "22:00-24:00"
    };
    for (int i = 0; i < 12; i++) {
        GtkWidget *label = gtk_label_new(times[i]);
        gtk_widget_set_halign(label, GTK_ALIGN_START);
        gtk_widget_add_css_class(label, "review-option-text");
        gtk_grid_attach(GTK_GRID(grid), label, 0, i + 1, 1, 1);
    }

    // Add empty cells for the timetable
    for (int day = 1; day <= 7; day++) {
        for (int time = 1; time <= 12; time++) {
            GtkWidget *cell = gtk_label_new("");
            gtk_widget_set_halign(cell, GTK_ALIGN_CENTER);
            gtk_widget_add_css_class(cell, "review-item");
            gtk_grid_attach(GTK_GRID(grid), cell, day, time, 1, 1);
        }
    }

    // Wrap in a scrolled window
    GtkWidget *scrolled_window = gtk_scrolled_window_new();
    gtk_widget_add_css_class(scrolled_window, "card");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled_window), grid);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);

    return scrolled_window;
}

static void on_activate(GtkApplication *app, gpointer user_data) {
    AppData *app_data = g_new0(AppData, 1);
    app_data->habits = NULL;
    app_data->habit_widgets = NULL;

    // Initialize SQLite database
    if (sqlite3_open("habit_tracker.db", &app_data->db) != SQLITE_OK) {
        g_printerr("Cannot open database: %s\n", sqlite3_errmsg(app_data->db));
        cleanup_app_data(app_data);
        return;
    }

    // Create table
    const char *create_table_sql =
        "CREATE TABLE IF NOT EXISTS habit_tracking ("
        "date TEXT, habit_name TEXT, completed INTEGER, "
        "days TEXT, "
        "PRIMARY KEY (date, habit_name));";
    if (sqlite3_exec(app_data->db, create_table_sql, NULL, NULL, NULL) != SQLITE_OK) {
        g_printerr("Failed to create table: %s\n", sqlite3_errmsg(app_data->db));
        cleanup_app_data(app_data);
        return;
    }

    // Main window
    app_data->main_window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(app_data->main_window), "Habit Tracker");
    gtk_window_set_default_size(GTK_WINDOW(app_data->main_window), 1000, 800);
    gtk_window_set_icon_name(GTK_WINDOW(app_data->main_window), "emblem-favorite");

    // CSS styling (from quiz app)
    GtkCssProvider *css_provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css_provider,
        "window { background: linear-gradient(135deg, #f5f7fa 0%, #e4e7eb 100%); font-family: 'Roboto', sans-serif; color: #1f2937; transition: background 0.3s; }"
        "button { margin: 8px; padding: 12px 24px; border-radius: 8px; font-weight: 600; font-size: 14px; border: none; transition: all 0.3s ease; }"
        "button:hover { transform: scale(1.05); box-shadow: 0 4px 8px rgba(0,0,0,0.2); }"
        "button:active { transform: scale(0.95); }"
        "label { margin: 8px; font-size: 14px; }"
        ".title-1 { font-size: 32pt; font-weight: 700; margin: 20px 0; color: #1e40af; text-shadow: 0 2px 4px rgba(0,0,0,0.1); }"
        ".user-info { font-size: 16px; font-style: italic; color: #4b5563; margin-bottom: 20px; }"
        "entry { padding: 12px; margin: 8px; border-radius: 8px; border: 2px solid #d1d5db; font-size: 14px; background: white; transition: border-color 0.3s, box-shadow 0.3s; }"
        "entry:focus { border-color: #4f46e5; box-shadow: 0 0 8px rgba(79,70,229,0.5); }"
        ".entry-icon { margin: 12px 8px; opacity: 0.7; }"
        ".review-listbox { background: #ffffff; border-radius: 8px; padding: 12px; }"
        ".review-item { padding: 15px; margin-bottom: 12px; border-radius: 8px; border: 1px solid #e5e7eb; background: #f9fafb; }"
        ".review-option-text { margin-left: 20px; padding: 5px; border-radius: 5px; font-size: 14px; }"
        ".card { background: #ffffff; border-radius: 12px; box-shadow: 0 8px 16px rgba(0,0,0,0.15); padding: 20px; }"
        ".form-frame { border: none; background: transparent; }"
        "button.destructive-action { background: linear-gradient(90deg, #ef4444, #dc2626); color: white; }"
        "button.destructive-action:hover { background: linear-gradient(90deg, #dc2626, #b91c1c); }"
        "button.gradient-button { background: linear-gradient(90deg, #4f46e5, #7c3aed); color: white; }"
        "button.gradient-button:hover { background: linear-gradient(90deg, #7c3aed, #4f46e5); }"
        "dialog { background: #ffffff; border-radius: 12px; box-shadow: 0 8px 16px rgba(0,0,0,0.2); padding: 20px; }"
        "dialog label { font-size: 16px; font-weight: bold; color: #1e40af; }"
    );
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(),
        GTK_STYLE_PROVIDER(css_provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
    );
    g_object_unref(css_provider);

    // Header bar
    GtkWidget *header_bar = gtk_header_bar_new();
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header_bar), gtk_label_new("Habit Tracker"));
    GDateTime *date = g_date_time_new_now_local();
    char date_str[64];
    snprintf(date_str, sizeof(date_str), "Date: %s, %d/%d/%d",
             g_date_time_format(date, "%A"),
             g_date_time_get_day_of_month(date),
             g_date_time_get_month(date),
             g_date_time_get_year(date));
    g_date_time_unref(date);
    app_data->date_label = gtk_label_new(date_str);
    gtk_widget_add_css_class(app_data->date_label, "user-info");
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header_bar), app_data->date_label);
    gtk_window_set_titlebar(GTK_WINDOW(app_data->main_window), header_bar);

    // Main layout
    GtkWidget *stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);
    gtk_window_set_child(GTK_WINDOW(app_data->main_window), stack);

    // Habits page
    app_data->habits_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 20);
    gtk_widget_set_halign(app_data->habits_vbox, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(app_data->habits_vbox, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(app_data->habits_vbox, "card");

    GtkWidget *title = gtk_label_new("Habit Tracker");
    gtk_widget_add_css_class(title, "title-1");
    gtk_box_append(GTK_BOX(app_data->habits_vbox), title);

    GtkWidget *frame = gtk_frame_new(NULL);
    gtk_widget_add_css_class(frame, "form-frame");
    GtkWidget *inner_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 15);
    gtk_frame_set_child(GTK_FRAME(frame), inner_box);
    gtk_box_append(GTK_BOX(app_data->habits_vbox), frame);

    // Habits section
    app_data->habits_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_append(GTK_BOX(inner_box), app_data->habits_box);

    // Tasks section
    GtkWidget *tasks_label = gtk_label_new("Tasks");
    gtk_widget_add_css_class(tasks_label, "review-question-text");
    gtk_box_append(GTK_BOX(inner_box), tasks_label);

    GtkWidget *task1 = gtk_label_new("Finish project");
    gtk_widget_add_css_class(task1, "review-option-text");
    gtk_box_append(GTK_BOX(inner_box), task1);

    GtkWidget *task2 = gtk_label_new("Call a friend");
    gtk_widget_add_css_class(task2, "review-option-text");
    gtk_box_append(GTK_BOX(inner_box), task2);

    // Edit Habits button
    GtkWidget *edit_habits_button = gtk_button_new_with_label("Edit Habits");
    gtk_widget_add_css_class(edit_habits_button, "gradient-button");
    g_signal_connect(edit_habits_button, "clicked", G_CALLBACK(on_edit_habits), app_data);
    gtk_box_append(GTK_BOX(inner_box), edit_habits_button);

    gtk_stack_add_titled(GTK_STACK(stack), app_data->habits_vbox, "habits", "Habits");

    // Timetable page
    GtkWidget *timetable_page = create_timetable_page();
    gtk_stack_add_titled(GTK_STACK(stack), timetable_page, "timetable", "Timetable");

    // Cleanup on window destroy
    g_signal_connect_swapped(app_data->main_window, "destroy", G_CALLBACK(cleanup_app_data), app_data);

    // Show main window
    gtk_window_present(GTK_WINDOW(app_data->main_window));
}

int main(int argc, char *argv[]) {
    GtkApplication *app = gtk_application_new("com.habit.tracker", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}