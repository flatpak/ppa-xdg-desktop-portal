#include <config.h>

#include <gio/gio.h>

#include "src/xdp-dbus.h"

#ifdef HAVE_LIBPORTAL
#include "account.h"
#include "email.h"
#include "filechooser.h"
#include "screenshot.h"
#include "trash.h"
#endif

#define PORTAL_BUS_NAME "org.freedesktop.portal.Desktop"
#define PORTAL_OBJECT_PATH "/org/freedesktop/portal/desktop"
#define BACKEND_BUS_NAME "org.freedesktop.impl.portal.Test"

char outdir[] = "/tmp/xdp-test-XXXXXX";

static GTestDBus *dbus;
static GDBusConnection *session_bus;
static GSubprocess *portals;
static GSubprocess *backends;

static void
name_appeared_cb (GDBusConnection *bus,
                  const char *name,
                  const char *name_owner,
                  gpointer data)
{
  gboolean *b = data;

  g_debug ("Name %s now owned by %s\n", name, name_owner);

  *b = TRUE;

  g_main_context_wakeup (NULL);
}

static void
name_disappeared_cb (GDBusConnection *bus,
                     const char *name,
                     gpointer data)
{
  g_debug ("Name %s disappeared\n", name);
}

static gboolean
timeout_cb (gpointer data)
{
  const char *msg = data;

  g_error ("%s", msg);

  return G_SOURCE_REMOVE;
}

static void
global_setup (void)
{
  GError *error = NULL;
  g_autofree gchar *services = NULL;
  g_autofree gchar *portal_dir = NULL;
  g_autoptr(GSubprocessLauncher) launcher = NULL;
  gboolean name_appeared = FALSE;
  guint name_timeout;
  const char *argv[3];

  g_mkdtemp (outdir);
  g_print ("outdir: %s\n", outdir);

  g_setenv ("XDG_RUNTIME_DIR", outdir, TRUE);
  g_setenv ("XDG_DATA_HOME", outdir, TRUE);

  dbus = g_test_dbus_new (G_TEST_DBUS_NONE);
  services = g_test_build_filename (G_TEST_BUILT, "services", NULL);
  g_test_dbus_add_service_dir (dbus, services);
  g_test_dbus_up (dbus);

  /* g_test_dbus_up unsets this, so re-set */
  g_setenv ("XDG_RUNTIME_DIR", outdir, TRUE);

  session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  g_assert_no_error (error);

  /* start portal backends */
  g_bus_watch_name_on_connection (session_bus,
                                  BACKEND_BUS_NAME,
                                  0,
                                  name_appeared_cb,
                                  name_disappeared_cb,
                                  &name_appeared,
                                  NULL);

  launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_NONE);
  g_subprocess_launcher_setenv (launcher, "G_DEBUG", "fatal-criticals", TRUE);
  g_subprocess_launcher_setenv (launcher, "DBUS_SESSION_BUS_ADDRESS", g_test_dbus_get_bus_address (dbus), TRUE);
  g_subprocess_launcher_setenv (launcher, "XDG_DATA_HOME", outdir, TRUE);
 
  argv[0] = "./test-backends";
  argv[1] = g_test_verbose () ? "--verbose" : NULL;
  argv[2] = NULL;

  backends = g_subprocess_launcher_spawnv (launcher, argv, &error);
  g_assert_no_error (error);

  name_timeout = g_timeout_add (1000, timeout_cb, "Failed to launch test-backends");

  while (!name_appeared)
    g_main_context_iteration (NULL, TRUE);

  g_source_remove (name_timeout);

  name_appeared = FALSE;
  
  /* start portals */
  g_bus_watch_name_on_connection (session_bus,
                                  PORTAL_BUS_NAME,
                                  0,
                                  name_appeared_cb,
                                  name_disappeared_cb,
                                  &name_appeared,
                                  NULL);

  portal_dir = g_test_build_filename (G_TEST_DIST, "portals", NULL);

  launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_NONE);
  g_subprocess_launcher_setenv (launcher, "G_DEBUG", "fatal-criticals", TRUE);
  g_subprocess_launcher_setenv (launcher, "DBUS_SESSION_BUS_ADDRESS", g_test_dbus_get_bus_address (dbus), TRUE);
  g_subprocess_launcher_setenv (launcher, "XDG_DESKTOP_PORTAL_DIR", portal_dir, TRUE);
  g_subprocess_launcher_setenv (launcher, "XDG_DATA_HOME", outdir, TRUE);
 
  argv[0] = "./xdg-desktop-portal";
  argv[1] = g_test_verbose () ? "--verbose" : NULL;
  argv[2] = NULL;

  portals = g_subprocess_launcher_spawnv (launcher, argv, &error);
  g_assert_no_error (error);

  name_timeout = g_timeout_add (1000, timeout_cb, "Failed to launch xdg-desktop-portal");

  while (!name_appeared)
    g_main_context_iteration (NULL, TRUE);

  g_source_remove (name_timeout);
}

static void
global_teardown (void)
{
  GError *error = NULL;
  g_autoptr(GFile) outdir_file = g_file_new_for_path (outdir);

  g_dbus_connection_close_sync (session_bus, NULL, &error);
  g_assert_no_error (error);

  g_subprocess_force_exit (portals);
  g_subprocess_force_exit (backends);

  g_object_unref (session_bus);

  g_test_dbus_down (dbus);

  g_object_unref (dbus);
}

/* Just check that the portal is there, and has the
 * expected version. This will fail if the backend
 * is not found.
 */
static void
test_account_exists (void)
{
  g_autoptr(GDBusProxy) proxy = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *owner = NULL;

  proxy = G_DBUS_PROXY (xdp_account_proxy_new_sync (session_bus,
                                                    0,
                                                    PORTAL_BUS_NAME,
                                                    PORTAL_OBJECT_PATH,
                                                    NULL,
                                                    &error));
  g_assert_no_error (error);

  owner = g_dbus_proxy_get_name_owner (proxy);
  g_assert_nonnull (owner);

  g_assert_cmpuint (xdp_account_get_version (XDP_ACCOUNT (proxy)), ==, 1);
}

static void
test_email_exists (void)
{
  g_autoptr(GDBusProxy) proxy = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *owner = NULL;

  proxy = G_DBUS_PROXY (xdp_email_proxy_new_sync (session_bus,
                                                  0,
                                                  PORTAL_BUS_NAME,
                                                  PORTAL_OBJECT_PATH,
                                                  NULL,
                                                  &error));
  g_assert_no_error (error);

  owner = g_dbus_proxy_get_name_owner (proxy);
  g_assert_nonnull (owner);

  g_assert_cmpuint (xdp_email_get_version (XDP_EMAIL (proxy)), ==, 2);
}

static void
test_screenshot_exists (void)
{
  g_autoptr(GDBusProxy) proxy = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *owner = NULL;

  proxy = G_DBUS_PROXY (xdp_screenshot_proxy_new_sync (session_bus,
                                                       0,
                                                       PORTAL_BUS_NAME,
                                                       PORTAL_OBJECT_PATH,
                                                       NULL,
                                                       &error));
  g_assert_no_error (error);

  owner = g_dbus_proxy_get_name_owner (proxy);
  g_assert_nonnull (owner);

  g_assert_cmpuint (xdp_screenshot_get_version (XDP_SCREENSHOT (proxy)), ==, 2);
}

static void
test_trash_exists (void)
{
  g_autoptr(GDBusProxy) proxy = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *owner = NULL;

  proxy = G_DBUS_PROXY (xdp_trash_proxy_new_sync (session_bus,
                                                  0,
                                                  PORTAL_BUS_NAME,
                                                  PORTAL_OBJECT_PATH,
                                                  NULL,
                                                  &error));
  g_assert_no_error (error);

  owner = g_dbus_proxy_get_name_owner (proxy);
  g_assert_nonnull (owner);

  g_assert_cmpuint (xdp_trash_get_version (XDP_TRASH (proxy)), ==, 1);
}

static void
test_settings_exists (void)
{
  g_autoptr(GDBusProxy) proxy = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *owner = NULL;

  proxy = G_DBUS_PROXY (xdp_settings_proxy_new_sync (session_bus,
                                                     0,
                                                     PORTAL_BUS_NAME,
                                                     PORTAL_OBJECT_PATH,
                                                     NULL,
                                                     &error));
  g_assert_no_error (error);

  owner = g_dbus_proxy_get_name_owner (proxy);
  g_assert_nonnull (owner);

  g_assert_cmpuint (xdp_settings_get_version (XDP_SETTINGS (proxy)), ==, 1);
}

static void
test_proxy_resolver_exists (void)
{
  g_autoptr(GDBusProxy) proxy = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *owner = NULL;

  proxy = G_DBUS_PROXY (xdp_proxy_resolver_proxy_new_sync (session_bus,
                                                           0,
                                                           PORTAL_BUS_NAME,
                                                           PORTAL_OBJECT_PATH,
                                                           NULL,
                                                           &error));
  g_assert_no_error (error);

  owner = g_dbus_proxy_get_name_owner (proxy);
  g_assert_nonnull (owner);

  g_assert_cmpuint (xdp_proxy_resolver_get_version (XDP_PROXY_RESOLVER (proxy)), ==, 1);
}

static void
test_network_monitor_exists (void)
{
  g_autoptr(GDBusProxy) proxy = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *owner = NULL;

  proxy = G_DBUS_PROXY (xdp_network_monitor_proxy_new_sync (session_bus,
                                                            0,
                                                            PORTAL_BUS_NAME,
                                                            PORTAL_OBJECT_PATH,
                                                            NULL,
                                                            &error));
  g_assert_no_error (error);

  owner = g_dbus_proxy_get_name_owner (proxy);
  g_assert_nonnull (owner);

  g_assert_cmpuint (xdp_network_monitor_get_version (XDP_NETWORK_MONITOR (proxy)), ==, 3);
}

static void
test_file_chooser_exists (void)
{
  g_autoptr(GDBusProxy) proxy = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *owner = NULL;

  proxy = G_DBUS_PROXY (xdp_file_chooser_proxy_new_sync (session_bus,
                                                         0,
                                                         PORTAL_BUS_NAME,
                                                         PORTAL_OBJECT_PATH,
                                                         NULL,
                                                         &error));
  g_assert_no_error (error);

  owner = g_dbus_proxy_get_name_owner (proxy);
  g_assert_nonnull (owner);

  g_assert_cmpuint (xdp_file_chooser_get_version (XDP_FILE_CHOOSER (proxy)), ==, 1);
}

int
main (int argc, char **argv)
{
  int res;

  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/portal/account/exists", test_account_exists);
  g_test_add_func ("/portal/email/exists", test_email_exists);
  g_test_add_func ("/portal/filechooser/exists", test_file_chooser_exists);
  g_test_add_func ("/portal/networkmonitor/exists", test_network_monitor_exists);
  g_test_add_func ("/portal/proxyresolver/exists", test_proxy_resolver_exists);
  g_test_add_func ("/portal/screenshot/exists", test_screenshot_exists);
  g_test_add_func ("/portal/settings/exists", test_settings_exists);
  g_test_add_func ("/portal/trash/exists", test_trash_exists);

#ifdef HAVE_LIBPORTAL
  g_test_add_func ("/portal/account/basic", test_account_libportal);
  g_test_add_func ("/portal/account/delay", test_account_delay);
  g_test_add_func ("/portal/account/cancel", test_account_cancel);
  g_test_add_func ("/portal/account/close", test_account_close);
  g_test_add_func ("/portal/account/reason", test_account_reason);

  g_test_add_func ("/portal/email/basic", test_email_libportal);
  g_test_add_func ("/portal/email/delay", test_email_delay);
  g_test_add_func ("/portal/email/cancel", test_email_cancel);
  g_test_add_func ("/portal/email/close", test_email_close);
  g_test_add_func ("/portal/email/address", test_email_address);
  g_test_add_func ("/portal/email/subject", test_email_subject);

  g_test_add_func ("/portal/screenshot/basic", test_screenshot_libportal);
  g_test_add_func ("/portal/screenshot/delay", test_screenshot_delay);
  g_test_add_func ("/portal/screenshot/cancel", test_screenshot_cancel);
  g_test_add_func ("/portal/screenshot/close", test_screenshot_close);

  g_test_add_func ("/portal/color/basic", test_color_libportal);
  g_test_add_func ("/portal/color/delay", test_color_delay);
  g_test_add_func ("/portal/color/cancel", test_color_cancel);
  g_test_add_func ("/portal/color/close", test_color_close);

  g_test_add_func ("/portal/trash/file", test_trash_file);

  g_test_add_func ("/portal/openfile/basic", test_open_file_libportal);
  g_test_add_func ("/portal/openfile/delay", test_open_file_delay);
  g_test_add_func ("/portal/openfile/close", test_open_file_close);
  g_test_add_func ("/portal/openfile/cancel", test_open_file_cancel);
#endif

  global_setup ();

  res = g_test_run ();

  global_teardown ();

  return res;
}

