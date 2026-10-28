/* SPDX-License-Identifier: Zlib */

#include <girara-gtk/settings.h>
#include <girara/log.h>

#include <errno.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <limits.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#include "zathura.h"
#include "plugin.h"
#include "utils.h"
#ifdef __APPLE__
#include "document.h"
#include "commands.h"
#include "callbacks.h"
#include "shortcuts.h"
#include <mach-o/dyld.h>
#include <gio/gunixinputstream.h>
#endif
#ifdef WITH_SECCOMP
#include "seccomp-filters.h"
#endif
#ifdef WITH_LANDLOCK
#include "landlock.h"
#endif
/* SyncTeX needs D-Bus, which the sandbox disables, so it is unavailable there. */
#if defined(WITH_SYNCTEX) && !defined(WITH_SANDBOX)
#include "dbus-interface.h"
#include "synctex.h"
#endif

#ifdef __APPLE__
/* Silence this GTK warning without hiding other diagnostics. */
static GLogWriterOutput filter_skipped_frame_warning(GLogLevelFlags level, const GLogField* fields,
                                                    gsize n_fields, gpointer user_data) {
  const char* domain = NULL;
  const char* message = NULL;
  for (gsize i = 0; i < n_fields; ++i) {
    if (fields[i].length != -1) {
      continue;
    }
    if (g_strcmp0(fields[i].key, "GLIB_DOMAIN") == 0) {
      domain = fields[i].value;
    } else if (g_strcmp0(fields[i].key, "MESSAGE") == 0) {
      message = fields[i].value;
    }
  }
  if ((level & G_LOG_LEVEL_MASK) == G_LOG_LEVEL_WARNING && !(level & G_LOG_FLAG_FATAL) &&
      g_strcmp0(domain, "Gdk") == 0 &&
      g_strcmp0(message, "gdk_frame_timings_presented() called on skipped frame.") == 0) {
    return G_LOG_WRITER_HANDLED;
  }
  return g_log_writer_default(level, fields, n_fields, user_data);
}
#endif

#if defined(WITH_SYNCTEX) && !defined(WITH_SANDBOX)
/* Handle synctex forward synchronization */
static int run_synctex_forward(const char* synctex_fwd, const char* filename, int synctex_pid) {
  g_autoptr(GFile) file = g_file_new_for_commandline_arg(filename);
  if (!file) {
    girara_error("Unable to handle argument '%s'.", filename);
    return -1;
  }

  g_autofree char* real_path = g_file_get_path(file);
  if (!real_path) {
    girara_error("Failed to determine path for '%s'", filename);
    return -1;
  }

  int line                    = 0;
  int column                  = 0;
  g_autofree char* input_file = NULL;
  if (!synctex_parse_input(synctex_fwd, &input_file, &line, &column)) {
    girara_error("Failed to parse argument to --synctex-forward.");
    return -1;
  }

  const int ret = zathura_dbus_synctex_position(real_path, input_file, line, column, synctex_pid);
  if (ret == -1) {
    /* D-Bus or SyncTeX failed */
    girara_error("Got no usable data from SyncTeX or D-Bus failed in some way.");
  }

  return ret;
}
#endif

static void init_locale(void) {
  setlocale(LC_ALL, "");
  bindtextdomain(GETTEXT_PACKAGE, LOCALEDIR);
  bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
  textdomain(GETTEXT_PACKAGE);
}

static zathura_t* init_zathura(const char* config_dir, const char* data_dir, const char* cache_dir,
                               const char* plugin_path, char** argv, const char* synctex_editor) {
  /* create zathura session */
  zathura_t* zathura = zathura_create();
  if (!zathura) {
    return NULL;
  }

  zathura_set_config_dir(zathura, config_dir);
  zathura_set_data_dir(zathura, data_dir);
  zathura_set_cache_dir(zathura, cache_dir);
  zathura_set_plugin_dir(zathura, plugin_path);
  zathura_set_argv(zathura, argv);

  /* Init zathura */
  if (!zathura_init(zathura)) {
    zathura_free(zathura);
    return NULL;
  }

  if (synctex_editor) {
    girara_setting_set(zathura->ui.session, "synctex-editor-command", synctex_editor);
  }

#ifdef WITH_SANDBOX
  girara_debug("Strict sandbox preventing write and network access.");
#ifdef WITH_LANDLOCK
  if (landlock_drop_write() < 0) {
    girara_error("Failed to apply landlock write restriction.");
    zathura_free(zathura);
    return NULL;
  }
#endif
#ifdef WITH_SECCOMP
  if (seccomp_enable_strict_filter(zathura)) {
    girara_error("Failed to initialize strict seccomp filter.");
    zathura_free(zathura);
    return NULL;
  }
#endif
#ifdef __OpenBSD__
  if (pledge("stdio rpath", "")) {
    girara_error("Failed to pledge: %s", strerror(errno));
    zathura_free(zathura);
    return NULL;
  }
#endif
#endif

  return zathura;
}

/* state shared between the application lifecycle callbacks */
typedef struct {
  const char* config_dir;
  const char* data_dir;
  const char* cache_dir;
  const char* plugin_path;
  const char* synctex_editor;
  const char* password;
  const char* synctex_fwd;
  const char* mode;
  const char* bookmark_name;
  const char* search_string;
  /* file argument exactly as given on the command line */
  const char* raw_file;
  int page_number;
  char** argv;
  zathura_t* zathura;
} zathura_app_ctx_t;

#ifndef __APPLE__
static void cb_app_startup(GApplication* app, gpointer data) {
  zathura_app_ctx_t* ctx = data;

  ctx->zathura =
      init_zathura(ctx->config_dir, ctx->data_dir, ctx->cache_dir, ctx->plugin_path, ctx->argv, ctx->synctex_editor);
  if (!ctx->zathura) {
    girara_error("Could not initialize zathura.");
    g_application_quit(app);
    return;
  }

  gtk_application_add_window(GTK_APPLICATION(app), GTK_WINDOW(ctx->zathura->ui.session->gtk.window));
}

static void cb_app_activate(GApplication* UNUSED(app), gpointer data) {
  /* present the window when the app starts without a file */
  zathura_app_ctx_t* ctx = data;
  if (ctx->zathura && ctx->zathura->ui.session && ctx->zathura->ui.session->gtk.window) {
    gtk_window_present(GTK_WINDOW(ctx->zathura->ui.session->gtk.window));
  }
}

static void cb_app_open(GApplication* UNUSED(app), GFile** files, gint n_files, const gchar* UNUSED(hint),
                        gpointer data) {
  zathura_app_ctx_t* ctx = data;
  if (!ctx->zathura || n_files < 1) {
    return;
  }

  /* gfile turns a plain dash into an absolute path which breaks stdin so keep the raw argument */
  g_autofree char* gfile_path = NULL;
  const char* path            = NULL;
  if (!g_strcmp0(ctx->raw_file, "-")) {
    path = "-";
  } else {
    gfile_path = g_file_get_path(files[0]);
    /* g_file_get_path returns NULL for non-local URIs; fall back to the raw argument */
    path = gfile_path ? gfile_path : ctx->raw_file;
  }
  if (!path) {
    girara_error("Failed to determine path for the given file.");
    return;
  }

  int page_number = ctx->page_number;
  if (page_number > 0) {
    --page_number;
  }
  document_open_idle(ctx->zathura, path, ctx->password, page_number, ctx->mode, ctx->synctex_fwd, ctx->bookmark_name,
                     ctx->search_string);
}

static void cb_app_shutdown(GApplication* UNUSED(app), gpointer data) {
  zathura_app_ctx_t* ctx = data;
  if (ctx->zathura) {
    zathura_free(ctx->zathura);
    ctx->zathura = NULL;
  }
}

#endif

static GStrv build_argv_for_child(int idx, char** argv, int argc, char** orig_argv, int orig_argc, int file_idx_base) {
  GPtrArray* arr = g_ptr_array_new();
  g_ptr_array_add(arr, g_strdup(orig_argv[0]));
  for (int i = 1; i < orig_argc; i++) {
    if (!g_strcmp0(orig_argv[i], "--fork")) {
      continue;
    }
    bool is_file = false;
    for (int j = 0; idx >= 0 && j < argc - file_idx_base && !is_file; j++) {
      if (!g_strcmp0(orig_argv[i], argv[file_idx_base + j])) {
        is_file = true;
      }
    }
    if (!is_file) {
      g_ptr_array_add(arr, g_strdup(orig_argv[i]));
    }
  }
  if (idx > 0) {
    g_ptr_array_add(arr, g_strdup(argv[idx]));
  }
  g_ptr_array_add(arr, NULL);
  return (GStrv)g_ptr_array_free(arr, FALSE);
}

static void start_process_group(void* GIRARA_UNUSED(data)) {
  if (setsid() == -1) {
    girara_error("Could not start new process group: %s", strerror(errno));
    exit(-1);
  }
}

#ifdef __APPLE__
/* Options travel with each open request, not with the host's first document. */
static void macos_open(GApplication* app, GFile** files, gint count, const gchar* hint, gpointer data) {
  zathura_app_ctx_t* defaults = data;
  g_autoptr(GVariant) options = NULL;
  if (hint && *hint) {
    options = g_variant_parse(G_VARIANT_TYPE_VARDICT, hint, NULL, NULL, NULL);
  }
  zathura_app_ctx_t request = {.plugin_path = defaults->plugin_path, .argv = defaults->argv,
                               .page_number = ZATHURA_PAGE_NUMBER_UNSPECIFIED};
  if (options) {
#define READ_OPTION(name, field) g_variant_lookup(options, name, "&s", &request.field)
    READ_OPTION("config", config_dir);
    READ_OPTION("data", data_dir);
    READ_OPTION("cache", cache_dir);
    READ_OPTION("plugins", plugin_path);
    READ_OPTION("editor", synctex_editor);
    READ_OPTION("password", password);
    READ_OPTION("synctex", synctex_fwd);
    READ_OPTION("mode", mode);
    READ_OPTION("bookmark", bookmark_name);
    READ_OPTION("find", search_string);
#undef READ_OPTION
    g_variant_lookup(options, "page", "i", &request.page_number);
  }

  for (int i = 0; i < count; ++i) {
    g_autofree char* path = g_file_get_path(files[i]);
    if (!path) {
      path = g_file_get_uri(files[i]);
    }
    zathura_t* viewer = NULL;
    zathura_t* empty = NULL;
    for (GList* item = gtk_application_get_windows(GTK_APPLICATION(app)); item; item = item->next) {
      zathura_t* candidate = g_object_get_data(item->data, "zathura-session");
      if (!candidate || g_object_get_data(item->data, "zathura-closing")) {
        continue;
      }
      if (zathura_has_document(candidate)) {
        g_autoptr(GFile) current = g_file_new_for_path(zathura_document_get_path(candidate->document));
        if (g_file_equal(current, files[i])) {
          viewer = candidate;
          break;
        }
      } else if (!g_object_get_data(item->data, "zathura-opening")) {
        empty = candidate;
      } else if (g_strcmp0(g_object_get_data(item->data, "zathura-opening"), path) == 0) {
        viewer = candidate;
        break;
      }
    }
    /* Only reuse a blank window when no custom session directories were supplied. */
    if (!viewer && empty && !request.config_dir && !request.data_dir && !request.cache_dir) {
      viewer = empty;
    }
    if (!viewer) {
      viewer = init_zathura(request.config_dir, request.data_dir, request.cache_dir,
                            request.plugin_path, defaults->argv, request.synctex_editor);
      if (!viewer) {
        girara_error("Could not initialise document window.");
        continue;
      }
      GtkWindow* window = GTK_WINDOW(viewer->ui.session->gtk.window);
      g_object_set_data(G_OBJECT(window), "zathura-session", viewer);
      gtk_application_add_window(GTK_APPLICATION(app), window);
    }
    if (request.synctex_editor) {
      girara_setting_set(viewer->ui.session, "synctex-editor-command", request.synctex_editor);
    }
    GtkWindow* window = GTK_WINDOW(viewer->ui.session->gtk.window);
    const char* cwd = NULL;
    if (options) {
      g_variant_lookup(options, "cwd", "&s", &cwd);
    }
    if (cwd || !g_object_get_data(G_OBJECT(window), "zathura-cwd")) {
      g_object_set_data_full(G_OBJECT(window), "zathura-cwd",
                              cwd ? g_strdup(cwd) : g_path_get_dirname(path), g_free);
    }
    if (!zathura_has_document(viewer) && !g_object_get_data(G_OBJECT(window), "zathura-opening")) {
      g_object_set_data_full(G_OBJECT(window), "zathura-opening", g_strdup(path), g_free);
      g_auto(GStrv) temporary = NULL;
      if (options && g_variant_lookup(options, "temporary-files", "^as", &temporary) &&
          g_strv_contains((const gchar* const*)temporary, path)) {
        viewer->stdin_support.file = g_strdup(path);
      }
      document_open_idle(viewer, path, request.password,
                          request.page_number > 0 ? request.page_number - 1 : request.page_number,
                          request.mode, request.synctex_fwd, request.bookmark_name, request.search_string);
    } else if (zathura_has_document(viewer)) {
      if (request.synctex_fwd) {
        int line = 0, column = 0;
        g_autofree char* input = NULL;
        if (synctex_parse_input(request.synctex_fwd, &input, &line, &column)) {
          synctex_view(viewer, input, line, column);
        }
      } else if (request.page_number != ZATHURA_PAGE_NUMBER_UNSPECIFIED) {
        int page = request.page_number > 0 ? request.page_number - 1 : request.page_number;
        if (page < 0) {
          page += zathura_document_get_number_of_pages(viewer->document);
        }
        page_set(viewer, page);
      }
      if (request.search_string) {
        girara_argument_t argument = {.n = FORWARD};
        cmd_search(viewer->ui.session, request.search_string, &argument);
      }
    }
    if (!zathura_has_document(viewer) && request.synctex_fwd) {
      g_object_set_data_full(G_OBJECT(window), "zathura-pending-forward",
                              g_strdup(request.synctex_fwd), g_free);
    }
    gtk_window_present(window);
  }
}

static void macos_activate(GApplication* app, gpointer data) {
  GtkWindow* window = gtk_application_get_active_window(GTK_APPLICATION(app));
  if (!window) {
    zathura_app_ctx_t* ctx = data;
    zathura_t* viewer = init_zathura(ctx->config_dir, ctx->data_dir, ctx->cache_dir,
                                    ctx->plugin_path, ctx->argv, ctx->synctex_editor);
    if (!viewer) {
      return;
    }
    window = GTK_WINDOW(viewer->ui.session->gtk.window);
    g_object_set_data(G_OBJECT(window), "zathura-session", viewer);
    gtk_application_add_window(GTK_APPLICATION(app), window);
  }
  gtk_window_present(window);
}

static void macos_shutdown(GApplication* app, gpointer UNUSED(data)) {
  while (gtk_application_get_windows(GTK_APPLICATION(app))) {
    GtkWindow* window = gtk_application_get_windows(GTK_APPLICATION(app))->data;
    zathura_t* viewer = g_object_get_data(G_OBJECT(window), "zathura-session");
    if (viewer) {
      zathura_free(viewer);
    } else {
      gtk_window_destroy(window);
    }
  }
}

static void macos_action(GSimpleAction* action, GVariant* UNUSED(parameter), gpointer data) {
  GtkApplication* app = data;
  if (g_strcmp0(g_action_get_name(G_ACTION(action)), "quit") == 0) {
    g_application_quit(G_APPLICATION(app));
  } else {
    GtkWindow* window = gtk_application_get_active_window(app);
    if (window) {
      gtk_window_close(window);
    }
  }
}

static void macos_startup(GApplication* app, gpointer UNUSED(data)) {
  const GActionEntry actions[] = {
      {.name = "quit", .activate = macos_action},
      {.name = "close", .activate = macos_action},
  };
  g_action_map_add_action_entries(G_ACTION_MAP(app), actions, G_N_ELEMENTS(actions), app);
  const char* quit_keys[] = {"<Meta>q", NULL};
  const char* close_keys[] = {"<Meta>w", NULL};
  gtk_application_set_accels_for_action(GTK_APPLICATION(app), "app.quit", quit_keys);
  gtk_application_set_accels_for_action(GTK_APPLICATION(app), "app.close", close_keys);
  g_autoptr(GMenu) menu = g_menu_new();
  g_autoptr(GMenu) application_menu = g_menu_new();
  g_menu_append(application_menu, "Close Window", "app.close");
  g_menu_append(application_menu, "Quit Zathura", "app.quit");
  g_menu_append_submenu(menu, "Zathura", G_MENU_MODEL(application_menu));
  gtk_application_set_menubar(GTK_APPLICATION(app), G_MENU_MODEL(menu));
}

static int macos_command_line(GApplication* UNUSED(app), GApplicationCommandLine* UNUSED(command),
                               gpointer UNUSED(data)) {
  /* The validated request was already delivered by g_application_open(). */
  return 0;
}

static int run_macos_application(zathura_app_ctx_t* ctx, int count, char** paths) {
  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
#define ADD_OPTION(name, field) \
  if (ctx->field) g_variant_builder_add(&builder, "{sv}", name, g_variant_new_string(ctx->field))
  ADD_OPTION("config", config_dir);
  ADD_OPTION("data", data_dir);
  ADD_OPTION("cache", cache_dir);
  ADD_OPTION("plugins", plugin_path);
  ADD_OPTION("editor", synctex_editor);
  ADD_OPTION("password", password);
  ADD_OPTION("synctex", synctex_fwd);
  ADD_OPTION("mode", mode);
  ADD_OPTION("bookmark", bookmark_name);
  ADD_OPTION("find", search_string);
#undef ADD_OPTION
  g_variant_builder_add(&builder, "{sv}", "page", g_variant_new_int32(ctx->page_number));
  g_autofree char* cwd = g_get_current_dir();
  g_variant_builder_add(&builder, "{sv}", "cwd", g_variant_new_string(cwd));

  GVariantBuilder temporary;
  g_variant_builder_init(&temporary, G_VARIANT_TYPE_STRING_ARRAY);
  g_autoptr(GPtrArray) files = g_ptr_array_new_with_free_func(g_object_unref);
  for (int i = 0; i < count; ++i) {
    if (g_strcmp0(paths[i], "-") == 0) {
      g_autoptr(GFileIOStream) stream = NULL;
      g_autoptr(GFile) file = g_file_new_tmp("zathura.stdin.XXXXXX", &stream, NULL);
      g_autoptr(GInputStream) input = g_unix_input_stream_new(STDIN_FILENO, FALSE);
      if (!file || g_output_stream_splice(g_io_stream_get_output_stream(G_IO_STREAM(stream)), input,
                                           G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET, NULL, NULL) < 0) {
        girara_error("Could not spool standard input.");
        return 1;
      }
      g_ptr_array_add(files, g_object_ref(file));
      g_autofree char* temporary_path = g_file_get_path(file);
      g_variant_builder_add(&temporary, "s", temporary_path);
    } else {
      g_ptr_array_add(files, g_file_new_for_commandline_arg(paths[i]));
    }
  }
  g_variant_builder_add(&builder, "{sv}", "temporary-files", g_variant_builder_end(&temporary));
  g_autoptr(GVariant) options = g_variant_ref_sink(g_variant_builder_end(&builder));
  g_autofree char* hint = g_variant_print(options, TRUE);
  g_autoptr(GtkApplication) app = gtk_application_new("org.pwmt.zathura", G_APPLICATION_HANDLES_OPEN | G_APPLICATION_HANDLES_COMMAND_LINE);
  g_signal_connect(app, "command-line", G_CALLBACK(macos_command_line), ctx);
  g_signal_connect(app, "startup", G_CALLBACK(macos_startup), ctx);
  g_signal_connect(app, "activate", G_CALLBACK(macos_activate), ctx);
  g_signal_connect(app, "open", G_CALLBACK(macos_open), ctx);
  g_signal_connect(app, "shutdown", G_CALLBACK(macos_shutdown), ctx);
  g_autoptr(GError) error = NULL;
  if (!g_application_register(G_APPLICATION(app), NULL, &error)) {
    girara_error("Could not register application: %s", error->message);
    return 1;
  }
  if (files->len) {
    g_application_open(G_APPLICATION(app), (GFile**)files->pdata, files->len, hint);
  } else {
    g_application_activate(G_APPLICATION(app));
  }
  char* run_argv[] = {ctx->argv[0], NULL};
  return g_application_run(G_APPLICATION(app), 1, run_argv);
}

/* Keep the actual Mach-O viewer as CFBundleExecutable, even for CLI launches. */
static void macos_bootstrap(int argc, char** argv) {
  char executable[PATH_MAX];
  uint32_t size = sizeof(executable);
  if (_NSGetExecutablePath(executable, &size) != 0) {
    return;
  }
  g_autofree char* resolved = realpath(executable, NULL);
  if (!resolved) {
    return;
  }
  g_autofree char* directory = g_path_get_dirname(resolved);
  g_autofree char* launcher = g_build_filename(directory, "../Resources/launcher.py", NULL);
  if (!g_file_test(launcher, G_FILE_TEST_IS_EXECUTABLE)) {
    return;
  }
  g_autofree char** args = g_new0(char*, argc + 1);
  args[0] = launcher;
  for (int i = 1; i < argc; ++i) {
    args[i] = argv[i];
  }
  g_autofree char* environment = NULL;
  g_autoptr(GError) error = NULL;
  int status = 0;
  if (!g_spawn_sync(NULL, args, NULL, 0, NULL, NULL, &environment, NULL, &status, &error) ||
      !g_spawn_check_wait_status(status, &error)) {
    g_printerr("Zathura runtime setup: %s\n", error->message);
    exit(EXIT_FAILURE);
  }
  for (char* entry = environment; *entry; entry += strlen(entry) + 1) {
    char* separator = strchr(entry, '=');
    if (separator) {
      g_autofree char* key = g_strndup(entry, separator - entry);
      g_setenv(key, separator + 1, TRUE);
    }
  }
}
#endif

/* main function */
GIRARA_VISIBLE int main(int argc, char* argv[]) {
#ifdef __APPLE__
  macos_bootstrap(argc, argv);
  g_log_set_writer_func(filter_skipped_frame_warning, NULL, NULL);
#endif
  init_locale();

#ifdef WITH_SANDBOX
  girara_debug("Running zathura-sandbox, disable IPC services");
  /* Prevent default gtk dbus connection */
  g_setenv("DBUS_SESSION_BUS_ADDRESS", "disabled:", TRUE);

  /* disable GTK4 accessibility bus connection */
  g_setenv("GTK_A11Y", "none", TRUE);

  /* disable dconf writing - uses /var/empty as alternative to /dev/null to avoid ioctl call */
  g_setenv("DCONF_PROFILE", "/var/empty", TRUE);

  /* use the software renderer to avoid the gpu stack attack surface (and high risk syscalls) */
  g_setenv("GSK_RENDERER", "cairo", TRUE);

  /* unset the input method to avoid communication with external services */
  unsetenv("GTK_IM_MODULE");
#endif

  /* parse command line arguments */
  g_autofree gchar* config_dir     = NULL;
  g_autofree gchar* data_dir       = NULL;
  g_autofree gchar* cache_dir      = NULL;
  g_autofree gchar* plugin_path    = NULL;
  g_autofree gchar* loglevel       = NULL;
  g_autofree gchar* password       = NULL;
  g_autofree gchar* synctex_editor = NULL;
  g_autofree gchar* synctex_fwd    = NULL;
  g_autofree gchar* mode           = NULL;
  g_autofree gchar* bookmark_name  = NULL;
  g_autofree gchar* search_string  = NULL;
  gboolean forkback                = false;
  gboolean print_version           = false;
  gint page_number                 = ZATHURA_PAGE_NUMBER_UNSPECIFIED;
  gint synctex_pid                 = -1;

  const GOptionEntry entries[] = {
      {"config-dir", 'c', 0, G_OPTION_ARG_FILENAME, &config_dir, _("Path to the config directory"), "path"},
      {"data-dir", 'd', 0, G_OPTION_ARG_FILENAME, &data_dir, _("Path to the data directory"), "path"},
      {"cache-dir", '\0', 0, G_OPTION_ARG_FILENAME, &cache_dir, _("Path to the cache directory"), "path"},
      {"plugins-dir", 'p', 0, G_OPTION_ARG_STRING, &plugin_path, _("Path to the directories containing plugins"),
       "path"},
      {"fork", '\0', 0, G_OPTION_ARG_NONE, &forkback, _("Fork into the background"), NULL},
      {"password", 'w', 0, G_OPTION_ARG_STRING, &password, _("Document password"), "password"},
      {"page", 'P', 0, G_OPTION_ARG_INT, &page_number, _("Page number to go to"), "number"},
      {"log-level", 'l', 0, G_OPTION_ARG_STRING, &loglevel, _("Log level (debug, info, warning, error)"), "level"},
      {"version", 'v', 0, G_OPTION_ARG_NONE, &print_version, _("Print version information"), NULL},
      {"synctex-editor-command", 'x', 0, G_OPTION_ARG_STRING, &synctex_editor,
       _("SyncTeX editor (forwarded to the synctex command)"), "cmd"},
      {"synctex-forward", '\0', 0, G_OPTION_ARG_STRING, &synctex_fwd, _("Move to given SyncTeX position"), "position"},
      {"synctex-pid", '\0', 0, G_OPTION_ARG_INT, &synctex_pid, _("Highlight given position in the given process"),
       "pid"},
      {"mode", '\0', 0, G_OPTION_ARG_STRING, &mode, _("Start in a non-default mode"), "mode"},
      {"bookmark", 'b', 0, G_OPTION_ARG_STRING, &bookmark_name, _("Bookmark to go to"), "bookmark"},
      {"find", 'f', 0, G_OPTION_ARG_STRING, &search_string, _("Search for the given phrase and display results"),
       "string"},
      {NULL, '\0', 0, 0, NULL, NULL, NULL},
  };

  g_autoptr(GOptionContext) context = g_option_context_new(" [file1] [file2] [...]");
  g_option_context_add_main_entries(context, entries, NULL);

  const int orig_argc     = argc;
  g_auto(GStrv) orig_argv = g_strdupv(argv);

  g_autoptr(GError) error = NULL;
  if (!g_option_context_parse(context, &argc, &argv, &error)) {
    girara_error("Error parsing command line arguments: %s\n", error->message);
    return -1;
  }

#ifdef __APPLE__
  if (!plugin_path) {
    plugin_path = g_strdup(g_getenv("ZATHURA_PLUGIN_PATH"));
  }
  if (!synctex_editor) {
    synctex_editor = g_strdup(g_getenv("ZATHURA_SYNCTEX_EDITOR"));
  }
#endif
  girara_set_log_level_from_string(loglevel);

#if defined(WITH_SYNCTEX) && !defined(WITH_SANDBOX)
  /* macOS routes ordinary forward searches through the document host. */
  if (synctex_fwd
#ifdef __APPLE__
      && synctex_pid != -1
#endif
  ) {
    if (argc != 2) {
      girara_error("Too many arguments or missing filename while running with "
                   "--synctex-forward");
      return -1;
    }

    int ret = run_synctex_forward(synctex_fwd, argv[1], synctex_pid);
    if (ret > 0) {
      /* Instance found. */
      return 0;
    } else if (ret < 0) {
      /* Error occurred. */
      return -1;
    }

    girara_debug("No instance found. Starting new one.");
  }
#else
  if (synctex_fwd || synctex_editor || synctex_pid != -1) {
    girara_error("Built without synctex support, but synctex specific option was specified.");
    return -1;
  }
#endif

  /* check mode */
  if (mode && g_strcmp0(mode, "presentation") && g_strcmp0(mode, "fullscreen")) {
    girara_error("Invalid argument for --mode: %s", mode);
    return -1;
  }

  /* g_option_context_parse has some funny (documented) behavior:
   * * for "-- a b c" you get no -- in argv
   * * for "-- --" you get -- in argv twice
   * * for "-- -a" you get -- in argv
   *
   * So if there is one -- in argv, we need to ignore it. */
  const bool has_double_dash = argc > 1 && !g_strcmp0(argv[1], "--");
  const int file_idx_base    = has_double_dash ? 2 : 1;

  int file_idx = argc > file_idx_base ? file_idx_base : 0;
#ifndef __APPLE__
  /* If more than one file, fork an instance for each. */
  if (!print_version && argc > file_idx_base + 1) {
    g_autoptr(girara_list_t) child_pids = girara_list_new();

    for (int idx = file_idx_base; idx < argc; ++idx) {
      g_auto(GStrv) spawn_argv = build_argv_for_child(idx, argv, argc, orig_argv, orig_argc, file_idx_base);
      GPid pid;
      if (!g_spawn_async(NULL, spawn_argv, NULL,
                         G_SPAWN_SEARCH_PATH | (forkback ? G_SPAWN_DEFAULT : G_SPAWN_DO_NOT_REAP_CHILD),
                         forkback ? start_process_group : NULL, NULL, &pid, &error)) {
        girara_error("Could not spawn: %s", error->message);
        return -1;
      }

      girara_list_append(child_pids, (void*)(intptr_t)pid);
    }

    if (!forkback) {
      for (size_t idx = 0; idx != girara_list_size(child_pids); ++idx) {
        const pid_t pid = (pid_t)(intptr_t)girara_list_nth(child_pids, idx);
        waitpid(pid, NULL, 0);
      }
    }
    return 0;
  }

#endif

  /* Fork into the background if the user really wants to ... */
  if (!print_version && forkback && file_idx < file_idx_base + 1) {
#ifdef __APPLE__
    const int child_file = -1; /* Forward all files to the shared host. */
#else
    const int child_file = file_idx;
#endif
    g_auto(GStrv) spawn_argv = build_argv_for_child(child_file, argv, argc, orig_argv, orig_argc, file_idx_base);
    if (!g_spawn_async(NULL, spawn_argv, NULL, G_SPAWN_DEFAULT | G_SPAWN_SEARCH_PATH, start_process_group, NULL, NULL,
                       &error)) {
      girara_error("Could not spawn: %s", error->message);
      return -1;
    }

    return 0;
  }

  /* Print version */
  if (print_version) {
    g_autoptr(zathura_plugin_manager_t) plugin_manager = zathura_plugin_manager_new();
    zathura_plugin_manager_set_dir(plugin_manager, plugin_path);
    zathura_plugin_manager_load(plugin_manager);

    g_autofree char* string = zathura_get_version_string(plugin_manager, false);
    if (string) {
      fprintf(stdout, "%s\n", string);
    }
    return 0;
  }

  /* fail early so these errors still exit with an error status */
  if (!file_idx) {
    if (bookmark_name) {
      girara_error("Can not use bookmark argument when no file is given");
      return -1;
    }
    if (search_string) {
      girara_error("Can not use find argument when no file is given");
      return -1;
    }
  }

  /* run zathura as a GtkApplication */
  zathura_app_ctx_t ctx = {
      .config_dir     = config_dir,
      .data_dir       = data_dir,
      .cache_dir      = cache_dir,
      .plugin_path    = plugin_path,
      .synctex_editor = synctex_editor,
      .password       = password,
      .synctex_fwd    = synctex_fwd,
      .mode           = mode,
      .bookmark_name  = bookmark_name,
      .search_string  = search_string,
      .raw_file       = NULL,
      .page_number    = page_number,
      .argv           = argv,
      .zathura        = NULL,
  };

#ifdef __APPLE__
  return run_macos_application(&ctx, argc - file_idx_base, argv + file_idx_base);
#else
  /* a NULL application id keeps the process non-unique and skips D-Bus registration */
  g_autoptr(GtkApplication) app = gtk_application_new(NULL, G_APPLICATION_NON_UNIQUE | G_APPLICATION_HANDLES_OPEN);
  g_signal_connect(app, "startup", G_CALLBACK(cb_app_startup), &ctx);
  g_signal_connect(app, "activate", G_CALLBACK(cb_app_activate), &ctx);
  g_signal_connect(app, "open", G_CALLBACK(cb_app_open), &ctx);
  g_signal_connect(app, "shutdown", G_CALLBACK(cb_app_shutdown), &ctx);

  /* feed g_application_run a minimal argv so it routes via open or activate */
  char* run_argv[3] = {argv[0], NULL, NULL};
  int run_argc      = 1;
  if (file_idx) {
    ctx.raw_file = argv[file_idx];
    run_argv[1]  = argv[file_idx];
    run_argc     = 2;
  }

  return g_application_run(G_APPLICATION(app), run_argc, run_argv);
#endif
}
