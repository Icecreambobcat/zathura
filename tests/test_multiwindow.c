/* SPDX-License-Identifier: Zlib */
#include <glib/gstdio.h>
#include <string.h>
#include <girara-gtk/settings.h>
#include "zathura.h"
#include "callbacks.h"
#include "document.h"
#include "document-widget.h"
#include "page-widget.h"
#include "synctex.h"

static char* directory;
static const char* executable_path;

static zathura_t* new_viewer(void) {
  zathura_t* viewer = zathura_create();
  zathura_set_config_dir(viewer, directory);
  zathura_set_data_dir(viewer, directory);
  zathura_set_cache_dir(viewer, directory);
  const char* plugins = g_getenv("ZATHURA_TEST_PLUGINS");
  if (plugins) {
    zathura_set_plugin_dir(viewer, plugins);
  }
  g_assert_true(zathura_init(viewer));
  return viewer;
}

static void test_independent_sessions(void) {
  zathura_t* first = new_viewer();
  zathura_t* second = new_viewer();
  g_assert_true(first->ui.session != second->ui.session);
  g_assert_true(first->ui.session->global.data == first);
  g_assert_true(second->ui.session->global.data == second);
  int pages = 3;
  girara_setting_set(first->ui.session, "pages-per-row", &pages);
  girara_setting_get(second->ui.session, "pages-per-row", &pages);
  g_assert_cmpint(pages, ==, 1);
  zathura_free(first);
  g_assert_nonnull(second->ui.session->gtk.window);
  zathura_free(second);
}

static void test_cancel_pending_open(void) {
  zathura_t* viewer = new_viewer();
  document_open_idle(viewer, "/nonexistent-test.pdf", NULL, 0, NULL, NULL, NULL, NULL);
  guint source = viewer->signals.open_document;
  g_assert_nonnull(g_main_context_find_source_by_id(NULL, source));
  document_open_idle(viewer, "/replacement-test.pdf", NULL, 0, NULL, NULL, NULL, NULL);
  g_assert_null(g_main_context_find_source_by_id(NULL, source));
  source = viewer->signals.open_document;
  zathura_free(viewer);
  g_assert_null(g_main_context_find_source_by_id(NULL, source));
}

static void check_inverse(zathura_t* viewer, const char* pdf, const char* label) {
  g_autofree char* source = g_strdup(pdf);
  strcpy(source + strlen(source) - 3, "tex");
  unsigned int page = 0;
  g_autoptr(girara_list_t) rectangles =
      synctex_rectangles_from_position(viewer, pdf, source, 4, 0, &page, NULL);
  g_assert_nonnull(rectangles);
  g_assert_cmpuint(girara_list_size(rectangles), >, 0);
  g_assert_cmpuint(page, ==, 1);
  zathura_rectangle_t* rect = girara_list_nth(rectangles, 0);
  double x = (rect->x1 + rect->x2) / 2;
  double y = (rect->y1 + rect->y2) / 2;
  g_autofree char* input = NULL;
  unsigned int line = 0;
  g_assert_true(synctex_get_input_line_column(viewer, pdf, page, x, y, &input, &line, NULL));
  g_assert_cmpstr(input, ==, source);
  g_assert_cmpuint(line, >=, 3);

  g_autofree char* output = g_build_filename(directory, label, NULL);
  g_autofree char* quoted_executable = g_shell_quote(executable_path);
  g_autofree char* quoted_output = g_shell_quote(output);
  g_autofree char* editor =
      g_strdup_printf("%s --record %s '%%{input}' '%%{line}'", quoted_executable, quoted_output);
  bool enabled = true;
  girara_setting_set(viewer->ui.session, "synctex", &enabled);
  girara_setting_set(viewer->ui.session, "synctex-editor-command", editor);
  zathura_document_widget_ensure_page(viewer->ui.document_widget, page);
  GtkWidget* widget = zathura_document_widget_get_page(viewer->ui.document_widget, page);
  scaled_button_release_event_t event = {
      .x = x, .y = y, .button = GDK_BUTTON_PRIMARY, .state = GDK_CONTROL_MASK};
  cb_page_widget_scaled_button_release(ZATHURA_PAGE_WIDGET(widget), &event, viewer);
  const gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
  while (!g_file_test(output, G_FILE_TEST_EXISTS) && g_get_monotonic_time() < deadline) {
    g_main_context_iteration(NULL, FALSE);
    g_usleep(10000);
  }
  g_autofree char* recorded = NULL;
  g_assert_true(g_file_get_contents(output, &recorded, NULL, NULL));
  g_autofree char* expected = g_strdup_printf("%s\n%u\n", source, line);
  g_assert_cmpstr(recorded, ==, expected);
  g_remove(output);
}

static void test_two_documents(void) {
  const char* one = g_getenv("ZATHURA_TEST_PDF_ONE");
  const char* two = g_getenv("ZATHURA_TEST_PDF_TWO");
  if (!one || !two) {
    g_test_skip("Set ZATHURA_TEST_PDF_ONE/TWO to two-page LaTeX fixtures with SyncTeX.");
    return;
  }
  zathura_t* first = new_viewer();
  zathura_t* second = new_viewer();
  girara_notify(first->ui.session, GIRARA_WARNING, "No document opened.");
  g_assert_true(gtk_widget_get_visible(first->ui.session->gtk.notification_area));
  g_assert_true(document_open(first, one, NULL, NULL, 0, NULL));
  g_assert_false(gtk_widget_get_visible(first->ui.session->gtk.notification_area));
  g_assert_true(document_open(second, two, NULL, NULL, 0, NULL));
  check_inverse(first, one, "inverse-one");
  check_inverse(second, two, "inverse-two");
  g_assert_true(page_set(second, 0));
  g_assert_true(page_set(first, 1));
  g_assert_cmpuint(zathura_document_get_current_page_number(second->document), ==, 0);
  zathura_free(first);
  g_assert_cmpstr(zathura_document_get_path(second->document), ==, two);
  g_assert_true(document_close(second, false));
  g_assert_true(document_open(second, two, NULL, NULL, 0, NULL));
  zathura_free(second);
}

int main(int argc, char** argv) {
  if (argc == 5 && g_strcmp0(argv[1], "--record") == 0) {
    g_autofree char* contents = g_strdup_printf("%s\n%s\n", argv[3], argv[4]);
    return g_file_set_contents(argv[2], contents, -1, NULL) ? 0 : 1;
  }
  executable_path = argv[0];
  g_test_init(&argc, &argv, NULL);
  gtk_init();
  directory = g_dir_make_tmp("zathura-multiwindow-XXXXXX", NULL);
  g_assert_nonnull(directory);
  g_autofree char* config = g_build_filename(directory, "zathurarc", NULL);
  g_assert_true(g_file_set_contents(config, "set database null\nset dbus-service false\n", -1, NULL));
  g_test_add_func("/multiwindow/sessions", test_independent_sessions);
  g_test_add_func("/multiwindow/cancel-open", test_cancel_pending_open);
  g_test_add_func("/multiwindow/documents-and-inverse-callback", test_two_documents);
  int result = g_test_run();
  g_remove(config);
  g_rmdir(directory);
  g_free(directory);
  return result;
}
