/***
  This file is part of cups-filters.

  This file is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.

  This file is distributed in the hope that it will be useful, but WITHOUT
  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
  or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General
  Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with avahi; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <ctype.h>
#include <errno.h>
#if defined(__OpenBSD__)
#include <sys/socket.h>
#endif /* __OpenBSD__ */
#include <sys/types.h>
#include <net/if.h>
#include <netinet/in.h>
#include <ifaddrs.h>
#include <resolv.h>
#include <stdio.h>
#include <sys/stat.h>
#include <assert.h>
#include <stdlib.h>
#include <time.h>
#include <signal.h>

#include <glib.h>

#ifdef HAVE_AVAHI
#include <avahi-client/client.h>
#include <avahi-client/lookup.h>

#include <avahi-glib/glib-watch.h>
#include <avahi-common/malloc.h>
#include <avahi-common/error.h>
#endif /* HAVE_AVAHI */

#include <gio/gio.h>

#include <cups/cups.h>
#include <cups/ppd.h>

/* Attribute to mark a CUPS queue as created by us */
#define CUPS_BROWSED_MARK "cups-browsed"

/* Timeout values in sec */
#define TIMEOUT_IMMEDIATELY -1
#define TIMEOUT_CONFIRM     10
#define TIMEOUT_RETRY       10
#define TIMEOUT_REMOVE      -1
#define TIMEOUT_CHECK_LIST   2

/* Status of remote printer */
typedef enum printer_status_e {
  STATUS_UNCONFIRMED = 0,	/* Generated in a previous session */
  STATUS_CONFIRMED,		/* Avahi confirms UNCONFIRMED printer */
  STATUS_TO_BE_CREATED,		/* Scheduled for creation */
  STATUS_BROWSE_PACKET_RECEIVED,/* Scheduled for creation with timeout */
  STATUS_DISAPPEARED		/* Scheduled for removal */
} printer_status_t;

/* Data structure for remote printers */
typedef struct remote_printer_s {
  char *name;
  char *uri;
  char *ppd;
  char *model;
  char *ifscript;
  printer_status_t status;
  time_t timeout;
  int duplicate;
  char *host;
  char *service_name;
  char *type;
  char *domain;
} remote_printer_t;

/* Data structure for network interfaces */
typedef struct netif_s {
  char *address;
  http_addr_t broadcast;
} netif_t;

/* Data structure for browse allow/deny rules */
typedef enum allow_type_e {
  ALLOW_IP,
  ALLOW_NET,
  ALLOW_INVALID
} allow_type_t;
typedef struct allow_s {
  allow_type_t type;
  http_addr_t addr;
  http_addr_t mask;
} allow_t;

/* Data struct for a printer discovered using BrowsePoll */
typedef struct browsepoll_printer_s {
  char *uri_supported;
  char *info;
} browsepoll_printer_t;

/* Data structure for a BrowsePoll server */
typedef struct browsepoll_s {
  char *server;
  int port;
  int major;
  int minor;
  gboolean can_subscribe;
  int subscription_id;
  int sequence_number;

  /* Remember which printers we discovered. This way we can just ask
   * if anything has changed, and if not we know these printers are
   * still there. */
  GList *printers; /* of browsepoll_printer_t */
} browsepoll_t;

/* Local printer (key is name) */
typedef struct local_printer_s {
  char *device_uri;
  gboolean cups_browsed_controlled;
} local_printer_t;

/* Browse data to send for local printer */
typedef struct browse_data_s {
  int type;
  int state;
  char *uri;
  char *location;
  char *info;
  char *make_model;
  char *browse_options;
} browse_data_t;

cups_array_t *remote_printers;
static cups_array_t *netifs;
static cups_array_t *browseallow;
static gboolean browseallow_all = FALSE;

static GHashTable *local_printers;
static browsepoll_t *local_printers_context = NULL;
static http_t *local_conn = NULL;
static gboolean inhibit_local_printers_update = FALSE;

static GList *browse_data = NULL;

static GMainLoop *gmainloop = NULL;
#ifdef HAVE_AVAHI
static AvahiGLibPoll *glib_poll = NULL;
static AvahiClient *client = NULL;
static AvahiServiceBrowser *sb1 = NULL, *sb2 = NULL;
#endif /* HAVE_AVAHI */
static guint queues_timer_id = (guint) -1;
static int browsesocket = -1;

#define BROWSE_DNSSD (1<<0)
#define BROWSE_CUPS  (1<<1)
static unsigned int BrowseLocalProtocols = 0;
static unsigned int BrowseRemoteProtocols = BROWSE_DNSSD;
static unsigned int BrowseInterval = 60;
static unsigned int BrowseTimeout = 300;
static uint16_t BrowsePort = 631;
static browsepoll_t **BrowsePoll = NULL;
static size_t NumBrowsePoll = 0;
static guint update_netifs_sourceid = -1;
static char *DomainSocket = NULL;
static unsigned int CreateIPPPrinterQueues = 0;
static int autoshutdown = 0;
static int autoshutdown_avahi = 0;
static int autoshutdown_timeout = 30;
static guint autoshutdown_exec_id = 0;

static int debug = 0;

static void recheck_timer (void);
static void browse_poll_create_subscription (browsepoll_t *context,
					     http_t *conn);
static gboolean browse_poll_get_notifications (browsepoll_t *context,
					       http_t *conn);
char            *_ppdCreateFromIPP(char *buffer, size_t bufsize, ipp_t *response);

#if (CUPS_VERSION_MAJOR > 1) || (CUPS_VERSION_MINOR > 5)
#define HAVE_CUPS_1_6 1
#endif

/*
 * CUPS 1.6 makes various structures private and
 * introduces these ippGet and ippSet functions
 * for all of the fields in these structures.
 * http://www.cups.org/str.php?L3928
 * We define (same signatures) our own accessors when CUPS < 1.6.
 */
#ifndef HAVE_CUPS_1_6
const char *
ippGetName(ipp_attribute_t *attr)
{
  return (attr->name);
}

ipp_op_t
ippGetOperation(ipp_t *ipp)
{
  return (ipp->request.op.operation_id);
}

ipp_status_t
ippGetStatusCode(ipp_t *ipp)
{
  return (ipp->request.status.status_code);
}

ipp_tag_t
ippGetGroupTag(ipp_attribute_t *attr)
{
  return (attr->group_tag);
}

ipp_tag_t
ippGetValueTag(ipp_attribute_t *attr)
{
  return (attr->value_tag);
}

int
ippGetCount(ipp_attribute_t *attr)
{
  return (attr->num_values);
}

int
ippGetInteger(ipp_attribute_t *attr,
              int             element)
{
  return (attr->values[element].integer);
}

int
ippGetBoolean(ipp_attribute_t *attr,
              int             element)
{
  return (attr->values[element].boolean);
}

const char *
ippGetString(ipp_attribute_t *attr,
             int             element,
             const char      **language)
{
  return (attr->values[element].string.text);
}

ipp_attribute_t	*
ippFirstAttribute(ipp_t *ipp)
{
  if (!ipp)
    return (NULL);
  return (ipp->current = ipp->attrs);
}

ipp_attribute_t *
ippNextAttribute(ipp_t *ipp)
{
  if (!ipp || !ipp->current)
    return (NULL);
  return (ipp->current = ipp->current->next);
}

int
ippSetVersion(ipp_t *ipp, int major, int minor)
{
  if (!ipp || major < 0 || minor < 0)
    return (0);
  ipp->request.any.version[0] = major;
  ipp->request.any.version[1] = minor;
  return (1);
}
#endif

void debug_printf(const char *format, ...) {
  if (debug) {
    va_list arglist;
    va_start(arglist, format);
    vfprintf(stderr, format, arglist);
    fflush(stderr);
    va_end(arglist);
  }
}

static const char *
password_callback (const char *prompt,
		   http_t *http,
		   const char *method,
		   const char *resource,
		   void *user_data)
{
  return NULL;
}

static http_t *
http_connect_local (void)
{
  if (!local_conn)
    local_conn = httpConnectEncrypt(cupsServer(), ippPort(),
				    cupsEncryption());

  return local_conn;
}

static void
http_close_local (void)
{
  if (local_conn) {
    httpClose (local_conn);
    local_conn = NULL;
  }
}

static local_printer_t *
new_local_printer (const char *device_uri,
		   gboolean cups_browsed_controlled)
{
  local_printer_t *printer = g_malloc (sizeof (local_printer_t));
  printer->device_uri = strdup (device_uri);
  printer->cups_browsed_controlled = cups_browsed_controlled;
  return printer;
}

static void
free_local_printer (gpointer data)
{
  local_printer_t *printer = data;
  free (printer->device_uri);
  free (printer);
}

static gboolean
local_printer_has_uri (gpointer key,
		       gpointer value,
		       gpointer user_data)
{
  local_printer_t *printer = value;
  char *device_uri = user_data;
  return g_str_equal (printer->device_uri, device_uri);
}

static void
local_printers_create_subscription (http_t *conn)
{
  if (!local_printers_context) {
    local_printers_context = g_malloc0 (sizeof (browsepoll_t));
    local_printers_context->server = "localhost";
    local_printers_context->port = BrowsePort;
    local_printers_context->can_subscribe = TRUE;
  }

  browse_poll_create_subscription (local_printers_context, conn);
}

static void
get_local_printers (void)
{
  cups_dest_t *dests = NULL;
  int num_dests = cupsGetDests (&dests);
  debug_printf ("cups-browsed [BrowsePoll localhost:631]: cupsGetDests\n");
  g_hash_table_remove_all (local_printers);
  for (int i = 0; i < num_dests; i++) {
    const char *val;
    cups_dest_t *dest = &dests[i];
    local_printer_t *printer;
    gboolean cups_browsed_controlled;
    const char *device_uri = cupsGetOption ("device-uri",
					    dest->num_options,
					    dest->options);
    val = cupsGetOption (CUPS_BROWSED_MARK,
			 dest->num_options,
			 dest->options);
    cups_browsed_controlled = val && (!strcasecmp (val, "yes") ||
				      !strcasecmp (val, "on") ||
				      !strcasecmp (val, "true"));
    printer = new_local_printer (device_uri,
				 cups_browsed_controlled);
    g_hash_table_insert (local_printers,
			 g_strdup (dest->name),
			 printer);
  }

  cupsFreeDests (num_dests, dests);
}

static browse_data_t *
new_browse_data (int type, int state, const gchar *uri,
		 const gchar *location, const gchar *info,
		 const gchar *make_model, const gchar *browse_options)
{
  browse_data_t *data = g_malloc (sizeof (browse_data_t));
  data->type = type;
  data->state = state;
  data->uri = g_strdup (uri);
  data->location = g_strdup (location);
  data->info = g_strdup (info);
  data->make_model = g_strdup (make_model);
  data->browse_options = g_strdup (browse_options);
  return data;
}

static void
browse_data_free (gpointer data)
{
  browse_data_t *bdata = data;
  g_free (bdata->uri);
  g_free (bdata->location);
  g_free (bdata->info);
  g_free (bdata->make_model);
  g_free (bdata->browse_options);
  g_free (bdata);
}

static void
prepare_browse_data (void)
{
  static const char * const rattrs[] = { "printer-type",
					 "printer-state",
					 "printer-uri-supported",
					 "printer-info",
					 "printer-location",
					 "printer-make-and-model",
					 "auth-info-required",
					 "printer-uuid",
					 "job-template" };
  ipp_t *request, *response = NULL;
  ipp_attribute_t *attr;
  http_t *conn = NULL;

  conn = http_connect_local ();

  if (conn == NULL) {
    debug_printf("cups-browsed: browse send failed to connect to localhost\n");
    goto fail;
  }

  request = ippNewRequest(CUPS_GET_PRINTERS);
  ippAddStrings (request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD,
		 "requested-attributes", sizeof (rattrs) / sizeof (rattrs[0]),
		 NULL, rattrs);
  ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_NAME,
		"requesting-user-name", NULL, cupsUser ());

  debug_printf("cups-browsed: preparing browse data\n");
  response = cupsDoRequest (conn, request, "/");
  if (cupsLastError() > IPP_OK_CONFLICT) {
    debug_printf("cups-browsed: browse send failed for localhost: %s\n",
		 cupsLastErrorString ());
    goto fail;
  }

  g_list_free_full (browse_data, browse_data_free);
  browse_data = NULL;
  for (attr = ippFirstAttribute(response); attr;
       attr = ippNextAttribute(response)) {
    int type = -1, state = -1;
    const char *uri = NULL;
    gchar *location = NULL;
    gchar *info = NULL;
    gchar *make_model = NULL;
    GString *browse_options = g_string_new ("");

    /* Skip any non-printer attributes */
    while (attr && ippGetGroupTag(attr) != IPP_TAG_PRINTER)
      attr = ippNextAttribute(response);

    if (!attr)
      break;

    while (attr && ippGetGroupTag(attr) == IPP_TAG_PRINTER) {
      const char *attrname = ippGetName(attr);
      int value_tag = ippGetValueTag(attr);

      if (!strcasecmp(attrname, "printer-type") &&
	  value_tag == IPP_TAG_ENUM) {
	type = ippGetInteger(attr, 0);
	if (type & CUPS_PRINTER_NOT_SHARED) {
	  /* Skip CUPS queues not marked as shared */
	  state = -1;
	  type = -1;
	  break;
	}
      } else if (!strcasecmp(attrname, "printer-state") &&
	       value_tag == IPP_TAG_ENUM)
	state = ippGetInteger(attr, 0);
      else if (!strcasecmp(attrname, "printer-uri-supported") &&
	       value_tag == IPP_TAG_URI)
	uri = ippGetString(attr, 0, NULL);
      else if (!strcasecmp(attrname, "printer-location") &&
	       value_tag == IPP_TAG_TEXT) {
	/* Remove quotes */
	gchar **tokens = g_strsplit (ippGetString(attr, 0, NULL), "\"", -1);
	location = g_strjoinv ("", tokens);
	g_strfreev (tokens);
      } else if (!strcasecmp(attrname, "printer-info") &&
		 value_tag == IPP_TAG_TEXT) {
	/* Remove quotes */
	gchar **tokens = g_strsplit (ippGetString(attr, 0, NULL), "\"", -1);
	info = g_strjoinv ("", tokens);
	g_strfreev (tokens);
      } else if (!strcasecmp(attrname, "printer-make-and-model") &&
		 value_tag == IPP_TAG_TEXT) {
	/* Remove quotes */
	gchar **tokens = g_strsplit (ippGetString(attr, 0, NULL), "\"", -1);
	make_model = g_strjoinv ("", tokens);
	g_strfreev (tokens);
      } else if (!strcasecmp(attrname, "auth-info-required") &&
		 value_tag == IPP_TAG_KEYWORD) {
	if (strcasecmp (ippGetString(attr, 0, NULL), "none"))
	  g_string_append_printf (browse_options, "auth-info-required=%s ",
				  ippGetString(attr, 0, NULL));
      } else if (!strcasecmp(attrname, "printer-uuid") &&
		 value_tag == IPP_TAG_URI)
	g_string_append_printf (browse_options, "uuid=%s ",
				ippGetString(attr, 0, NULL));
      else if (!strcasecmp(attrname, "job-sheets-default") &&
	       value_tag == IPP_TAG_NAME &&
	       ippGetCount(attr) == 2)
	g_string_append_printf (browse_options, "job-sheets=%s,%s ",
				ippGetString(attr, 0, NULL),
				ippGetString(attr, 1, NULL));
      else if (strstr(attrname, "-default")) {
	gchar *name = g_strdup (attrname);
	gchar *value = NULL;
	*strstr (name, "-default") = '\0';

	switch (value_tag) {
	  gchar **tokens;

	case IPP_TAG_KEYWORD:
	case IPP_TAG_STRING:
	case IPP_TAG_NAME:
	  /* Escape value */
	  tokens = g_strsplit_set (ippGetString(attr, 0, NULL),
				   " \"\'\\", -1);
	  value = g_strjoinv ("\\", tokens);
	  g_strfreev (tokens);
	  break;

	default:
	  /* other values aren't needed? */
	  debug_printf("cups-browsed: skipping %s (%d)\n", name, value_tag);
	  break;
	}

	if (value) {
	  g_string_append_printf (browse_options, "%s=%s ", name, value);
	  g_free (value);
	}

	g_free (name);
      }

      attr = ippNextAttribute(response);
    }

    if (type != -1 && state != -1 && uri && location && info && make_model) {
      gchar *browse_options_str = g_string_free (browse_options, FALSE);
      browse_data_t *data;
      browse_options = NULL;
      g_strchomp (browse_options_str);
      data = new_browse_data (type, state, uri, location,
			      info, make_model, browse_options_str);
      browse_data = g_list_insert (browse_data, data, 0);
      g_free (browse_options_str);
    }

    if (make_model)
      g_free (make_model);

    if (info)
      g_free (info);

    if (location)
      g_free (location);

    if (browse_options)
      g_string_free (browse_options, TRUE);

    if (!attr)
      break;
  }

 fail:
  if (response)
    ippDelete(response);
}

static void
update_local_printers (void)
{
  gboolean get_printers = FALSE;
  http_t *conn;

  if (inhibit_local_printers_update)
    return;

  conn = http_connect_local ();
  if (conn &&
      (!local_printers_context || local_printers_context->can_subscribe)) {
    if (!local_printers_context ||
	local_printers_context->subscription_id == -1) {
      /* No subscription yet. First, create the subscription. */
      local_printers_create_subscription (conn);
      get_printers = TRUE;
    } else
      /* We already have a subscription, so use it. */

      /* Note: for the moment, browse_poll_get_notifications() just
       * tells us whether we should re-fetch the printer list, so it
       * is safe to use here. */
      get_printers = browse_poll_get_notifications (local_printers_context,
						    conn);
  } else
    get_printers = TRUE;

  if (get_printers) {
    get_local_printers ();

    if (BrowseLocalProtocols & BROWSE_CUPS)
      prepare_browse_data ();
  }
}

gboolean
autoshutdown_execute (gpointer data)
{
  /* Are we still in auto shutdown mode and are we still without queues */
  if (autoshutdown && cupsArrayCount(remote_printers) == 0) {
    debug_printf("cups-browsed: Automatic shutdown as there are no print queues maintained by us for %d sec.\n",
		 autoshutdown_timeout);
    g_main_loop_quit(gmainloop);
  }

  /* Stop this timeout handler, we needed it only once */
  return FALSE;
}

static remote_printer_t *
create_local_queue (const char *name,
		    const char *uri,
		    const char *host,
		    const char *info,
		    const char *type,
		    const char *domain,
		    const char *pdl,
		    const char *make_model,
		    int is_cups_queue)
{
  remote_printer_t *p;
  remote_printer_t *q;
  int		fd = 0;			/* Script file descriptor */
  char		tempfile[1024];		/* Temporary file */
  char		buffer[8192];		/* Buffer for creating script */
  int           bytes;
  const char	*cups_serverbin;	/* CUPS_SERVERBIN environment variable */
  int uri_status, port;
  http_t *http;
  char scheme[10], userpass[1024], host_name[1024], resource[1024];
  ipp_t *request, *response;

  /* Mark this as a queue to be created locally pointing to the printer */
  if ((p = (remote_printer_t *)calloc(1, sizeof(remote_printer_t))) == NULL) {
    debug_printf("cups-browsed: ERROR: Unable to allocate memory.\n");
    return NULL;
  }

  /* Queue name */
  p->name = strdup(name);
  if (!p->name)
    goto fail;

  p->uri = strdup(uri);
  if (!p->uri)
    goto fail;

  p->host = strdup (host);
  if (!p->host)
    goto fail;

  p->service_name = strdup (info);
  if (!p->service_name)
    goto fail;

  /* Record Bonjour service parameters to identify print queue
     entry for removal when service disappears */
  p->type = strdup (type);
  if (!p->type)
    goto fail;

  p->domain = strdup (domain);
  if (!p->domain)
    goto fail;

  /* Schedule for immediate creation of the CUPS queue */
  p->status = STATUS_TO_BE_CREATED;
  p->timeout = time(NULL) + TIMEOUT_IMMEDIATELY;

  if (is_cups_queue) {
    /* Our local queue must be raw, so that the PPD file and driver
       on the remote CUPS server get used */
    p->ppd = NULL;
    p->model = NULL;
    p->ifscript = NULL;
    /* Check whether we have an equally named queue already from another
       server */
    for (q = (remote_printer_t *)cupsArrayFirst(remote_printers);
	 q;
	 q = (remote_printer_t *)cupsArrayNext(remote_printers))
      if (!strcasecmp(q->name, p->name))
	break;
    p->duplicate = (q && q->status != STATUS_DISAPPEARED &&
		    q->status != STATUS_UNCONFIRMED) ? 1 : 0;
    if (p->duplicate)
      debug_printf("cups-browsed: Printer %s already available through host %s.\n",
		   p->name, q->host);
    else if (q) {
      q->duplicate = 1;
      debug_printf("cups-browsed: Unconfirmed/disappeared printer %s already available through host %s, marking that printer duplicate of the newly found one.\n",
		   p->name, q->host);
    }
  } else {
    /* Non-CUPS printer broadcasts are most probably from printers
       directly connected to the network and using the IPP protocol.
       We check whether we can set them up without a device-specific
       driver, only using page description languages which the
       operating system provides: PCL 5c/5e/6/XL, PostScript, PDF, PWG
       Raster. Especially IPP Everywhere printers and PDF-capable 
       AirPrint printers will work this way. Making only driverless
       queues we can get an easy, configuration-less way to print
       from mobile devices, even if there is no CUPS server with
       shared printers around. */

    if (CreateIPPPrinterQueues == 0) {
      debug_printf("cups-browsed: Printer %s (%s) is an IPP network printer and cups-browsed is not configured to set up such printers automatically, ignoring this printer.\n",
		   p->name, p->uri);
      goto fail;
    }

    if (!pdl || pdl[0] == '\0' || (!strcasestr(pdl, "application/postscript") && !strcasestr(pdl, "application/pdf") && !strcasestr(pdl, "image/pwg-raster") && !strcasestr(pdl, "application/vnd.hp-PCL") && !strcasestr(pdl, "application/vnd.hp-PCLXL"))) {
      debug_printf("cups-browsed: Cannot create remote printer %s (%s) as its PDLs are not known, ignoring this printer.\n",
		   p->name, p->uri);
      goto fail;
    }

    p->duplicate = 0;
    p->model = NULL;

    /* Request printer properties and try to generate a PPD file for the
       printer (mainly IPP Everywhere printers) */
    uri_status = httpSeparateURI(HTTP_URI_CODING_ALL, uri,
				 scheme, sizeof(scheme),
				 userpass, sizeof(userpass),
				 host_name, sizeof(host_name),
				 &(port),
				 resource, sizeof(resource));
    if (uri_status != HTTP_URI_OK)
      goto fail;
    if ((http = httpConnect(host_name, port)) ==
	NULL) {
      debug_printf("cups-browsed: Cannot connect to remote printer %s (%s:%d), ignoring this printer.\n",
		   p->uri, host_name, port);
      goto fail;
    }
    request = ippNewRequest(IPP_OP_GET_PRINTER_ATTRIBUTES);
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, uri);
    response = cupsDoRequest(http, request, resource);

    if (!_ppdCreateFromIPP(buffer, sizeof(buffer), response)) {
      debug_printf("cups-browsed: Unable to create PPD file: %s\n", strerror(errno));
      p->ppd = NULL;

      if ((cups_serverbin = getenv("CUPS_SERVERBIN")) == NULL)
      cups_serverbin = CUPS_SERVERBIN;

      if ((fd = cupsTempFd(tempfile, sizeof(tempfile))) < 0) {
	debug_printf("Unable to create interface script file\n");
	goto fail;
      }
      
      debug_printf("Creating temp script file \"%s\"\n", tempfile);

      snprintf(buffer, sizeof(buffer),
	       "#!/bin/sh\n"
	       "# System V interface script for printer %s generated by cups-browsed\n"
	       "\n"
	       "if [ $# -lt 5 -o $# -gt 6 ]; then\n"
	       "  echo \"ERROR: $0 job-id user title copies options [file]\" >&2\n"
	       "  exit 1\n"
	       "fi\n"
	       "\n"
	       "# Read from given file\n"
	       "if [ -n \"$6\" ]; then\n"
	       "  exec \"$0\" \"$1\" \"$2\" \"$3\" \"$4\" \"$5\" < \"$6\"\n"
	       "fi\n"
	       "\n"
	       "extra_options=\"output-format=%s make-and-model=%s\"\n"
	       "\n"
	       "%s/filter/pdftoippprinter \"$1\" \"$2\" \"$3\" \"$4\" \"$5 $extra_options\"\n",
	       p->name, pdl, make_model, cups_serverbin);

      bytes = write(fd, buffer, strlen(buffer));
      if (bytes != strlen(buffer)) {
	debug_printf("Unable to write interface script into the file\n");
	goto fail;
      }

      close(fd);

      p->ifscript = strdup(tempfile);
    } else {
      debug_printf("cups-browsed: Created temporary IPP Everywhere PPD: %s\n", buffer);
      p->ppd = strdup(buffer);
      p->ifscript = NULL;
    }

    /*p->model = "drv:///sample.drv/laserjet.ppd";
      debug_printf("cups-browsed: PPD from system for %s: %s\n", p->name, p->model);*/

    /*p->ppd = "/usr/share/ppd/cupsfilters/pxlcolor.ppd";
      debug_printf("cups-browsed: PPD from file for %s: %s\n", p->name, p->ppd);*/

    /*p->ifscript = "/usr/lib/cups/filter/pdftoippprinter-wrapper";
      debug_printf("cups-browsed: System V Interface script for %s: %s\n", p->name, p->ifscript);*/

  }

  /* Add the new remote printer entry */
  cupsArrayAdd(remote_printers, p);

  /* If auto shutdown is active we have perhaps scheduled a timer to shut down
     due to not having queues any more to maintain, kill the timer now */
  if (autoshutdown && autoshutdown_exec_id &&
      cupsArrayCount(remote_printers) > 0) {
    debug_printf ("cups-browsed: New printers there to make available, killing auto shutdown timer.\n");
    g_source_remove(autoshutdown_exec_id);
    autoshutdown_exec_id = 0;
  }

  return p;

 fail:
  debug_printf("cups-browsed: ERROR: Unable to create print queue, ignoring printer.\n");
  free (p->type);
  free (p->service_name);
  free (p->host);
  free (p->uri);
  free (p->name);
  if (p->ppd) free (p->ppd);
  if (p->model) free (p->model);
  if (p->ifscript) free (p->ifscript);
  free (p);
  return NULL;
}

/*
 * Remove all illegal characters and replace each group of such characters
 * by a single dash, return a free()-able string.
 *
 * mode = 0: Only allow letters, numbers, dashes, and underscores for
 *           turning make/model info into a valid print queue name or
 *           into a string which can be supplied as option value in a
 *           filter command line without need of quoting
 * mode = 1: Allow also '/', '.', ',' for cleaning up MIME type
 *           strings (here available Page Description Languages, PDLs) to
 *           supply them on a filter command line without quoting
 *
 * Especially this prevents from arbitrary code execution by interface scripts
 * generated for print queues to native IPP printers when a malicious IPP
 * print service with forged PDL and/or make/model info gets broadcasted into
 * the local network.
 */

char *                                 /* O - Cleaned string */
remove_bad_chars(const char *str_orig, /* I - Original string */
		 int mode)             /* I - 0: Make/Model, queue name */
                                       /*     1: MIME types/PDLs */
{
  int i, j;
  int havedash = 0;
  char *str;

  if (str_orig == NULL)
    return NULL;

  str = strdup(str_orig);

  /* for later str[strlen(str)-1] access */
  if (strlen(str) < 1)
    return str;

  for (i = 0, j = 0; i < strlen(str); i++, j++) {
    if (((str[i] >= 'A') && (str[i] <= 'Z')) ||
	((str[i] >= 'a') && (str[i] <= 'z')) ||
	((str[i] >= '0') && (str[i] <= '9')) ||
	str[i] == '_' ||
	(mode == 1 && (str[i] == '/' ||
		       str[i] == '.' || str[i] == ','))) {
      /* Allowed character, keep it */
      havedash = 0;
      str[j] = str[i];
    } else {
      /* Replace all other characters by a single '-' */
      if (havedash == 1)
	j --;
      else {
	havedash = 1;
	str[j] = '-';
      }
    }
  }
  /* Add terminating zero */
  str[j] = '\0';
  /* Cut off trailing dashes */
  while (str[strlen(str)-1] == '-')
    str[strlen(str)-1] = '\0';

  /* Cut off leading dashes */
  i = 0;
  while (str[i] == '-')
    ++i;

  /* keep a free()-able string. +1 for trailing \0 */
  return memmove(str, str + i, strlen(str) - i + 1);
}

gboolean handle_cups_queues(gpointer unused) {
  remote_printer_t *p;
  http_t *http;
  char uri[HTTP_MAX_URI];
  int num_options;
  cups_option_t *options;
  int num_jobs;
  cups_job_t *jobs;
  ipp_t *request, *response;
  time_t current_time = time(NULL);
  const char *default_printer_name;
  ipp_attribute_t *attr;

  debug_printf("cups-browsed: Processing printer list ...\n");
  for (p = (remote_printer_t *)cupsArrayFirst(remote_printers);
       p; p = (remote_printer_t *)cupsArrayNext(remote_printers)) {
    switch (p->status) {

    /* Print queue generated by us in a previous session */
    case STATUS_UNCONFIRMED:

      /* Only act if the timeout has passed */
      if (p->timeout > current_time)
	break;

      /* Queue not reported again by Bonjour, remove it */
      p->status = STATUS_DISAPPEARED;
      p->timeout = current_time + TIMEOUT_IMMEDIATELY;

      debug_printf("cups-browsed: No remote printer named %s available, removing entry from previous session.\n",
		   p->name);

    /* Bonjour has reported this printer as disappeared or we have replaced
       this printer by another one */
    case STATUS_DISAPPEARED:

      /* Only act if the timeout has passed */
      if (p->timeout > current_time)
	break;

      debug_printf("cups-browsed: Removing entry %s%s.\n", p->name,
		   (p->duplicate ? "" : " and its CUPS queue"));

      /* Remove the CUPS queue */
      if (!p->duplicate) { /* Duplicates do not have a CUPS queue */
	if ((http = http_connect_local ()) == NULL) {
	  debug_printf("cups-browsed: Unable to connect to CUPS!\n");
	  p->timeout = current_time + TIMEOUT_RETRY;
	  break;
	}

	/* Check whether there are still jobs and do not remove the queue
	   then */
	num_jobs = 0;
	jobs = NULL;
	num_jobs = cupsGetJobs2(http, &jobs, p->name, 0, CUPS_WHICHJOBS_ACTIVE);
	if (num_jobs != 0) { /* error or jobs */
	  debug_printf("cups-browsed: Queue has still jobs or CUPS error!\n");
	  cupsFreeJobs(num_jobs, jobs);
	  /* Schedule the removal of the queue for later */
	  p->timeout = current_time + TIMEOUT_RETRY;
	  break;
	}

	/* Check whether the queue is the system default. In this case do not
	   remove it, so that this user setting does not get lost */
	default_printer_name = NULL;
	request = ippNewRequest(CUPS_GET_DEFAULT);
	/* Default user */
	ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME,
		     "requesting-user-name", NULL, cupsUser());
	/* Do it */
	response = cupsDoRequest(http, request, "/");
	if (cupsLastError() > IPP_OK_CONFLICT || !response) {
	  debug_printf("cups-browsed: Could not determine system default printer!\n");
	} else {
	  for (attr = ippFirstAttribute(response); attr != NULL;
	       attr = ippNextAttribute(response)) {
	    while (attr != NULL && ippGetGroupTag(attr) != IPP_TAG_PRINTER)
	      attr = ippNextAttribute(response);
	    if (attr) {
	      for (; attr && ippGetGroupTag(attr) == IPP_TAG_PRINTER;
		   attr = ippNextAttribute(response)) {
		if (!strcasecmp(ippGetName(attr), "printer-name") &&
		    ippGetValueTag(attr) == IPP_TAG_NAME) {
		  default_printer_name = ippGetString(attr, 0, NULL);
		  break;
		}
	      }
	    }
	    if (default_printer_name)
	      break;
	  }
	}
	if (default_printer_name &&
	    !strcasecmp(default_printer_name, p->name)) {
	  /* Printer is currently the system's default printer,
	     do not remove it */
	  /* Schedule the removal of the queue for later */
	  p->timeout = current_time + TIMEOUT_RETRY;
	  ippDelete(response);
	  break;
	}
	if (response)
	  ippDelete(response);

	/* No jobs, not default printer, remove the CUPS queue */
	request = ippNewRequest(CUPS_DELETE_PRINTER);
	/* Printer URI: ipp://localhost:631/printers/<queue name> */
	httpAssembleURIf(HTTP_URI_CODING_ALL, uri, sizeof(uri), "ipp", NULL,
			 "localhost", 0, "/printers/%s", p->name);
	ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI,
		     "printer-uri", NULL, uri);
	/* Default user */
	ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME,
		     "requesting-user-name", NULL, cupsUser());
	/* Do it */
	ippDelete(cupsDoRequest(http, request, "/admin/"));
	if (cupsLastError() > IPP_OK_CONFLICT) {
	  debug_printf("cups-browsed: Unable to remove CUPS queue!\n");
	  p->timeout = current_time + TIMEOUT_RETRY;
	  break;
	}
      }

      /* CUPS queue removed, remove the list entry */
      cupsArrayRemove(remote_printers, p);
      if (p->name) free (p->name);
      if (p->uri) free (p->uri);
      if (p->host) free (p->host);
      if (p->service_name) free (p->service_name);
      if (p->type) free (p->type);
      if (p->domain) free (p->domain);
      if (p->ppd) free (p->ppd);
      if (p->model) free (p->model);
      if (p->ifscript) free (p->ifscript);
      free(p);
      p = NULL;

      /* If auto shutdown is active and all printers we have set up got removed
	 again, schedule the shutdown in autoshutdown_timeout seconds */
      if (autoshutdown && !autoshutdown_exec_id &&
	  cupsArrayCount(remote_printers) == 0) {
	debug_printf ("cups-browsed: No printers there any more to make available, shutting down in %d sec...\n", autoshutdown_timeout);
	autoshutdown_exec_id =
	  g_timeout_add_seconds (autoshutdown_timeout, autoshutdown_execute,
				 NULL);
      }

      break;

    /* Bonjour has reported a new remote printer, create a CUPS queue for it,
       or upgrade an existing queue, or update a queue to use a backup host
       when it has disappeared on the currently used host */
    case STATUS_TO_BE_CREATED:
      /* (...or, we've just received a CUPS Browsing packet for this queue) */
    case STATUS_BROWSE_PACKET_RECEIVED:

      /* Do not create a queue for duplicates */
      if (p->duplicate) {
	p->timeout = (time_t) -1;
	break;
      }

      /* Only act if the timeout has passed */
      if (p->timeout > current_time)
	break;

      debug_printf("cups-browsed: Creating/Updating CUPS queue for %s\n",
		   p->name);

      /* Create a new CUPS queue or modify the existing queue */
      if ((http = http_connect_local ()) == NULL) {
	debug_printf("cups-browsed: Unable to connect to CUPS!\n");
	p->timeout = current_time + TIMEOUT_RETRY;
	break;
      }
      request = ippNewRequest(CUPS_ADD_MODIFY_PRINTER);
      /* Printer URI: ipp://localhost:631/printers/<queue name> */
      httpAssembleURIf(HTTP_URI_CODING_ALL, uri, sizeof(uri), "ipp", NULL,
		       "localhost", ippPort(), "/printers/%s", p->name);
      ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI,
               "printer-uri", NULL, uri);
      /* Default user */
      ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME,
		   "requesting-user-name", NULL, cupsUser());
      /* Queue should be enabled ... */
      ippAddInteger(request, IPP_TAG_PRINTER, IPP_TAG_ENUM, "printer-state",
		    IPP_PRINTER_IDLE);
      /* ... and accepting jobs */
      ippAddBoolean(request, IPP_TAG_PRINTER, "printer-is-accepting-jobs", 1);
      num_options = 0;
      options = NULL;
      /* Device URI: ipp(s)://<remote host>:631/printers/<remote queue> */
      num_options = cupsAddOption("device-uri", p->uri,
				  num_options, &options);
      /* Option cups-browsed=true, marking that we have created this queue */
      num_options = cupsAddOption(CUPS_BROWSED_MARK "-default", "true",
				  num_options, &options);
      /* Do not share a queue which serves only to point to a remote printer */
      num_options = cupsAddOption("printer-is-shared", "false",
				  num_options, &options);
      /* Description: <Bonjour service name> */
      num_options = cupsAddOption("printer-info", p->service_name,
				  num_options, &options);
      /* Location: <Remote host name> */
      num_options = cupsAddOption("printer-location", p->host,
				  num_options, &options);
      cupsEncodeOptions2(request, num_options, options, IPP_TAG_PRINTER);
      /* PPD from system's CUPS installation */
      if (p->model) {
	debug_printf("cups-browsed: Non-raw queue %s with system PPD: %s\n", p->name, p->model);
	p->ppd = cupsGetServerPPD(http, p->model);
      }
      /* Do it */
      if (p->ppd) {
	debug_printf("cups-browsed: Non-raw queue %s with PPD file: %s\n", p->name, p->ppd);
	ippDelete(cupsDoFileRequest(http, request, "/admin/", p->ppd));
	if (p->model) {
	  unlink(p->ppd);
	  free(p->ppd);
	  p->ppd = NULL;
	}
      } else if (p->ifscript) {
	debug_printf("cups-browsed: Non-raw queue %s with interface script: %s\n", p->name, p->ifscript);
	ippDelete(cupsDoFileRequest(http, request, "/admin/", p->ifscript));
	unlink(p->ifscript);
	free(p->ifscript);
	p->ifscript = NULL;
      } else {
	debug_printf("cups-browsed: Raw queue %s\n", p->name);
	ippDelete(cupsDoRequest(http, request, "/admin/"));
      }
      cupsFreeOptions(num_options, options);
      if (cupsLastError() > IPP_OK_CONFLICT) {
	debug_printf("cups-browsed: Unable to create CUPS queue!\n");
	p->timeout = current_time + TIMEOUT_RETRY;
	break;
      }

      if (p->status == STATUS_BROWSE_PACKET_RECEIVED) {
	p->status = STATUS_DISAPPEARED;
	p->timeout = time(NULL) + BrowseTimeout;
	debug_printf("cups-browsed: starting BrowseTimeout timer for %s (%ds)\n",
		     p->name, BrowseTimeout);
      } else {
	p->status = STATUS_CONFIRMED;
	p->timeout = (time_t) -1;
      }

      break;

    /* Nothing to do */
    case STATUS_CONFIRMED:
      break;

    }
  }

  recheck_timer ();

  /* Don't run this callback again */
  return FALSE;
}

static void
recheck_timer (void)
{
  remote_printer_t *p;
  time_t timeout = (time_t) -1;
  time_t now = time(NULL);

  if (!gmainloop)
    return;

  for (p = (remote_printer_t *)cupsArrayFirst(remote_printers);
       p;
       p = (remote_printer_t *)cupsArrayNext(remote_printers))
    if (p->timeout == (time_t) -1)
      continue;
    else if (now > p->timeout) {
      timeout = 0;
      break;
    } else if (timeout == (time_t) -1 || p->timeout - now < timeout)
      timeout = p->timeout - now;

  if (queues_timer_id != (guint) -1)
    g_source_remove (queues_timer_id);

  if (timeout != (time_t) -1) {
    queues_timer_id = g_timeout_add_seconds (timeout, handle_cups_queues, NULL);
    debug_printf("cups-browsed: checking queues in %ds\n", timeout);
  } else {
    queues_timer_id = (guint) -1;
    debug_printf("cups-browsed: listening\n");
  }
}

static remote_printer_t *
generate_local_queue(const char *host,
		     uint16_t port,
		     char *resource,
		     const char *name,
		     const char *type,
		     const char *domain,
		     void *txt) {

  char uri[HTTP_MAX_URI];
  char *remote_queue = NULL, *remote_host = NULL, *pdl = NULL;
#ifdef HAVE_AVAHI
  char *fields[] = { "product", "usb_MDL", "ty", NULL }, **f;
  AvahiStringList *entry = NULL;
  char *key = NULL, *value = NULL;
#endif /* HAVE_AVAHI */
  remote_printer_t *p;
  local_printer_t *local_printer;
  char *backup_queue_name = NULL, *local_queue_name = NULL;
  int is_cups_queue;
  size_t hl = 0;
  gboolean create = TRUE;
  

  is_cups_queue = 0;
  memset(uri, 0, sizeof(uri));

  /* Determine the device URI of the remote printer */
  httpAssembleURIf(HTTP_URI_CODING_ALL, uri, sizeof(uri) - 1,
		   (strcasestr(type, "_ipps") ? "ipps" : "ipp"), NULL,
		   host, port, "/%s", resource);
  /* Find the remote host name.
   * Used in constructing backup_queue_name, so need to sanitize.
   * strdup() is called inside remove_bad_chars() and result is free()-able.
   */
  remote_host = remove_bad_chars(host, 1);
  hl = strlen(remote_host);
  if (hl > 6 && !strcasecmp(remote_host + hl - 6, ".local"))
    remote_host[hl - 6] = '\0';
  if (hl > 7 && !strcasecmp(remote_host + hl - 7, ".local."))
    remote_host[hl - 7] = '\0';

  /* Check by the resource whether the discovered printer is a CUPS queue */
  if (!strncasecmp(resource, "printers/", 9)) {
    /* This is a remote CUPS queue, use the remote queue name for the
       local queue */
    is_cups_queue = 1;
    /* Not directly used in script generation input later, but taken from
       packet, so better safe than sorry. (consider second loop with
       backup_queue_name) */
    remote_queue = remove_bad_chars(resource + 9, 0);
    debug_printf("cups-browsed: Found CUPS queue: %s on host %s.\n",
		 remote_queue, remote_host);
#ifdef HAVE_AVAHI
    /* If the remote queue has a PPD file, the "product" field of the
       TXT record is populated. If it has no PPD file the remote queue
       is a raw queue and so we do not know enough about the printer
       behind it for auto-creating a local queue pointing to it. */
    int raw_queue = 0;
    if (txt) {
      entry = avahi_string_list_find((AvahiStringList *)txt, "product");
      if (entry) {
	avahi_string_list_get_pair(entry, &key, &value, NULL);
	if (!key || !value || strcasecmp(key, "product") || value[0] != '(' ||
	    value[strlen(value) - 1] != ')') {
	  raw_queue = 1;
	}
      } else
	raw_queue = 1;
    } else if (domain && domain[0] != '\0')
      raw_queue = 1;
    if (raw_queue) {
      /* The remote CUPS queue is raw, ignore it */
      debug_printf("cups-browsed: Remote Bonjour-advertised CUPS queue %s on host %s is raw, ignored.\n",
		   remote_queue, remote_host);
      free (remote_host);
      return NULL;
    }
#endif /* HAVE_AVAHI */
  } else if (!strncasecmp(resource, "classes/", 8)) {
    /* This is a remote CUPS queue, use the remote queue name for the
       local queue */
    is_cups_queue = 1;
    /* Not directly used in script generation input later, but taken from
       packet, so better safe than sorry. (consider second loop with
       backup_queue_name) */
    remote_queue = remove_bad_chars(resource + 8, 0);
    debug_printf("cups-browsed: Found CUPS queue: %s on host %s.\n",
		 remote_queue, remote_host);
  } else {
    /* This is an IPP-based network printer */
    is_cups_queue = 0;
    /* Determine the queue name by the model */
    remote_queue = strdup("printer");
#ifdef HAVE_AVAHI
    if (txt) {
      for (f = fields; *f; f ++) {
	entry = avahi_string_list_find((AvahiStringList *)txt, *f);
	if (entry) {
	  avahi_string_list_get_pair(entry, &key, &value, NULL);
	  if (key && value && !strcasecmp(key, *f) && strlen(value) >= 3) {
	    remote_queue = remove_bad_chars(value, 0);
	    break;
	  }
	}
      }
      /* Find out which PDLs the printer understands */
      entry = avahi_string_list_find((AvahiStringList *)txt, "pdl");
      if (entry) {
	avahi_string_list_get_pair(entry, &key, &value, NULL);
	if (key && value && !strcasecmp(key, "pdl") && strlen(value) >= 3) {
	  pdl = remove_bad_chars(value, 1);
	}
      }
    }
#endif /* HAVE_AVAHI */
  }
  /* Check if there exists already a CUPS queue with the
     requested name Try name@host in such a case and if
     this is also taken, ignore the printer */
  if ((backup_queue_name = malloc((strlen(remote_queue) +
				   strlen(remote_host) + 2) *
				  sizeof(char))) == NULL) {
    debug_printf("cups-browsed: ERROR: Unable to allocate memory.\n");
    exit(1);
  }
  sprintf(backup_queue_name, "%s@%s", remote_queue, remote_host);

  /* Get available CUPS queues */
  update_local_printers ();

  local_queue_name = remote_queue;

  /* Is there a local queue with the same URI as the remote queue? */
  if (g_hash_table_find (local_printers,
			 local_printer_has_uri,
			 uri))
    create = FALSE;

  if (create) {
    /* Is there a local queue with the name of the remote queue? */
    local_printer = g_hash_table_lookup (local_printers,
					 local_queue_name);
      /* Only consider CUPS queues not created by us */
    if (local_printer && !local_printer->cups_browsed_controlled) {
      /* Found local queue with same name as remote queue */
      /* Is there a local queue with the name <queue>@<host>? */
      local_queue_name = backup_queue_name;
      debug_printf("cups-browsed: %s already taken, using fallback name: %s\n",
		   remote_queue, local_queue_name);
      local_printer = g_hash_table_lookup (local_printers,
					   local_queue_name);
      if (local_printer && !local_printer->cups_browsed_controlled) {
	/* Found also a local queue with name <queue>@<host>, so
	   ignore this remote printer */
	debug_printf("cups-browsed: %s also taken, printer ignored.\n",
		     local_queue_name);
	free (backup_queue_name);
	free (remote_host);
	free (pdl);
	free (remote_queue);
	return NULL;
      }
    }
  }

  /* Check if we have already created a queue for the discovered
     printer */
  for (p = (remote_printer_t *)cupsArrayFirst(remote_printers);
       p; p = (remote_printer_t *)cupsArrayNext(remote_printers))
    if (!strcasecmp(p->name, local_queue_name) &&
	(p->host[0] == '\0' ||
	 p->status == STATUS_UNCONFIRMED ||
	 p->status == STATUS_DISAPPEARED ||
	 !strcasecmp(p->host, remote_host)))
      break;

  if (!create) {
    free (remote_host);
    free (backup_queue_name);
    free (pdl);
    free (remote_queue);
    if (p) {
      return p;
    } else {
      /* Found a local queue with the same URI as our discovered printer
	 would get, so ignore this remote printer */
      debug_printf("cups-browsed: Printer with URI %s already exists, printer ignored.\n",
		   uri);
      return NULL;
    }
  }

  if (p) {
    /* We have already created a local queue, check whether the
       discovered service allows us to upgrade the queue to IPPS
       or whether the URI part after ipp(s):// has changed */
    if ((strcasestr(type, "_ipps") &&
	 !strncasecmp(p->uri, "ipp:", 4)) ||
	strcasecmp(strchr(p->uri, ':'), strchr(uri, ':'))) {

      /* Schedule local queue for upgrade to ipps: or for URI change */
      if (strcasestr(type, "_ipps") &&
	  !strncasecmp(p->uri, "ipp:", 4))
	debug_printf("cups-browsed: Upgrading printer %s (Host: %s) to IPPS. New URI: %s\n",
		     p->name, remote_host, uri);
      if (strcasecmp(strchr(p->uri, ':'), strchr(uri, ':')))
	debug_printf("cups-browsed: Changing URI of printer %s (Host: %s) to %s.\n",
		     p->name, remote_host, uri);
      free(p->uri);
      free(p->host);
      free(p->service_name);
      free(p->type);
      free(p->domain);
      p->uri = strdup(uri);
      p->status = STATUS_TO_BE_CREATED;
      p->timeout = time(NULL) + TIMEOUT_IMMEDIATELY;
      p->host = strdup(remote_host);
      p->service_name = strdup(name);
      p->type = strdup(type);
      p->domain = strdup(domain);

    } else {

      /* Nothing to do, mark queue entry as confirmed if the entry
	 is unconfirmed */
      debug_printf("cups-browsed: Entry for %s (URI: %s) already exists.\n",
		   p->name, p->uri);
      if (p->status == STATUS_UNCONFIRMED ||
	  p->status == STATUS_DISAPPEARED) {
	p->status = STATUS_CONFIRMED;
	p->timeout = (time_t) -1;
	debug_printf("cups-browsed: Marking entry for %s (URI: %s) as confirmed.\n",
		     p->name, p->uri);
      }

    }
    if (p->host[0] == '\0') {
      free (p->host);
      p->host = strdup(remote_host);
    }
    if (p->service_name[0] == '\0' && name) {
      free (p->service_name);
      p->service_name = strdup(name);
    }
    if (p->type[0] == '\0' && type) {
      free (p->type);
      p->type = strdup(type);
    }
    if (p->domain[0] == '\0' && domain) {
      free (p->domain);
      p->domain = strdup(domain);
    }
  } else {

    /* We need to create a local queue pointing to the
       discovered printer */
    p = create_local_queue (local_queue_name, uri, remote_host,
			    name ? name : "", type, domain, pdl, remote_queue,
			    is_cups_queue);
  }

  free (backup_queue_name);
  free (remote_host);
  free (pdl);
  free (remote_queue);

  if (p)
    debug_printf("cups-browsed: Bonjour IDs: Service name: \"%s\", "
		 "Service type: \"%s\", Domain: \"%s\"\n",
		 p->service_name, p->type, p->domain);

  return p;
}

#ifdef HAVE_AVAHI
static void resolve_callback(
  AvahiServiceResolver *r,
  AVAHI_GCC_UNUSED AvahiIfIndex interface,
  AVAHI_GCC_UNUSED AvahiProtocol protocol,
  AvahiResolverEvent event,
  const char *name,
  const char *type,
  const char *domain,
  const char *host_name,
  const AvahiAddress *address,
  uint16_t port,
  AvahiStringList *txt,
  AvahiLookupResultFlags flags,
  AVAHI_GCC_UNUSED void* userdata) {

  assert(r);

  /* Called whenever a service has been resolved successfully or timed out */

  switch (event) {

  /* Resolver error */
  case AVAHI_RESOLVER_FAILURE:
    debug_printf("cups-browsed: Avahi-Resolver: Failed to resolve service '%s' of type '%s' in domain '%s': %s\n",
		 name, type, domain,
		 avahi_strerror(avahi_client_errno(avahi_service_resolver_get_client(r))));
    break;

  /* New remote printer found */
  case AVAHI_RESOLVER_FOUND: {
    AvahiStringList *rp_entry, *adminurl_entry;
    char *rp_key, *rp_value, *adminurl_key, *adminurl_value;

    debug_printf("cups-browsed: Avahi Resolver: Service '%s' of type '%s' in domain '%s'.\n",
		 name, type, domain);

    rp_entry = avahi_string_list_find(txt, "rp");
    if (rp_entry)
      avahi_string_list_get_pair(rp_entry, &rp_key, &rp_value, NULL);
    else {
      rp_key = strdup("rp");
      rp_value = strdup("");
    }
    adminurl_entry = avahi_string_list_find(txt, "adminurl");
    if (adminurl_entry)
      avahi_string_list_get_pair(adminurl_entry, &adminurl_key,
				 &adminurl_value, NULL);
    else {
      adminurl_key = strdup("adminurl");
      if ((adminurl_value = malloc(strlen(host_name) + 8)) != NULL)
	sprintf(adminurl_value, "http://%s", host_name);
      else
	adminurl_value = strdup("");
    }

    if (rp_key && rp_value && adminurl_key && adminurl_value &&
	!strcasecmp(rp_key, "rp") && !strcasecmp(adminurl_key, "adminurl")) {
      /* Check remote printer type and create appropriate local queue to
         point to it */
      generate_local_queue(host_name, port, rp_value, name, type, domain, txt);
    }

    /* Clean up */

    avahi_free(rp_key);
    avahi_free(rp_value);
    avahi_free(adminurl_key);
    avahi_free(adminurl_value);
    break;
  }
  }

  avahi_service_resolver_free(r);

  recheck_timer ();
}

static void browse_callback(
  AvahiServiceBrowser *b,
  AvahiIfIndex interface,
  AvahiProtocol protocol,
  AvahiBrowserEvent event,
  const char *name,
  const char *type,
  const char *domain,
  AvahiLookupResultFlags flags,
  void* userdata) {

  AvahiClient *c = userdata;
  assert(b);

  /* Called whenever a new services becomes available on the LAN or
     is removed from the LAN */

  switch (event) {

  /* Avahi browser error */
  case AVAHI_BROWSER_FAILURE:

    debug_printf("cups-browsed: Avahi Browser: ERROR: %s\n",
		 avahi_strerror(avahi_client_errno(avahi_service_browser_get_client(b))));
    g_main_loop_quit(gmainloop);
    return;

  /* New service (remote printer) */
  case AVAHI_BROWSER_NEW:

    /* Ignore events from the local machine */
    if (flags & AVAHI_LOOKUP_RESULT_LOCAL)
      break;

    debug_printf("cups-browsed: Avahi Browser: NEW: service '%s' of type '%s' in domain '%s'\n",
		 name, type, domain);

    /* We ignore the returned resolver object. In the callback
       function we free it. If the server is terminated before
       the callback function is called the server will free
       the resolver for us. */

    if (!(avahi_service_resolver_new(c, interface, protocol, name, type, domain, AVAHI_PROTO_UNSPEC, 0, resolve_callback, c)))
      debug_printf("Failed to resolve service '%s': %s\n",
		   name, avahi_strerror(avahi_client_errno(c)));
    break;

  /* A service (remote printer) has disappeared */
  case AVAHI_BROWSER_REMOVE: {
    remote_printer_t *p, *q;

    /* Ignore events from the local machine */
    if (flags & AVAHI_LOOKUP_RESULT_LOCAL)
      break;

    debug_printf("cups-browsed: Avahi Browser: REMOVE: service '%s' of type '%s' in domain '%s'\n",
		 name, type, domain);

    /* Check whether we have listed this printer */
    for (p = (remote_printer_t *)cupsArrayFirst(remote_printers);
	 p; p = (remote_printer_t *)cupsArrayNext(remote_printers))
      if (!strcasecmp(p->service_name, name) &&
	  !strcasecmp(p->type, type) &&
	  !strcasecmp(p->domain, domain))
	break;
    if (p) {
      /* Check whether this queue has a duplicate from another server */
      q = NULL;
      if (!p->duplicate) {
	for (q = (remote_printer_t *)cupsArrayFirst(remote_printers);
	     q;
	     q = (remote_printer_t *)cupsArrayNext(remote_printers))
	  if (!strcasecmp(q->name, p->name) &&
	      strcasecmp(q->host, p->host) &&
	      q->duplicate)
	    break;
      }
      if (q) {
	/* Remove the data of the disappeared remote printer */
	free (p->uri);
	free (p->host);
	free (p->service_name);
	free (p->type);
	free (p->domain);
	if (p->ppd) free (p->ppd);
	if (p->model) free (p->model);
	if (p->ifscript) free (p->ifscript);
	/* Replace the data with the data of the duplicate printer */
	p->uri = strdup(q->uri);
	p->host = strdup(q->host);
	p->service_name = strdup(q->service_name);
	p->type = strdup(q->type);
	p->domain = strdup(q->domain);
	if (q->ppd) p->ppd = strdup(q->ppd);
	if (q->model) p->model = strdup(q->model);
	if (q->ifscript) p->ifscript = strdup(q->ifscript);
	/* Schedule this printer for updating the CUPS queue */
	p->status = STATUS_TO_BE_CREATED;
	p->timeout = time(NULL) + TIMEOUT_IMMEDIATELY;
	/* Schedule the duplicate printer entry for removal */
	q->status = STATUS_DISAPPEARED;
	q->timeout = time(NULL) + TIMEOUT_IMMEDIATELY;

	debug_printf("cups-browsed: Printer %s diasappeared, replacing by backup on host %s with URI %s.\n",
		     p->name, p->host, p->uri);
      } else {

	/* Schedule CUPS queue for removal */
	p->status = STATUS_DISAPPEARED;
	p->timeout = time(NULL) + TIMEOUT_REMOVE;

	debug_printf("cups-browsed: Printer %s (Host: %s, URI: %s) disappeared and no backup available, removing entry.\n",
		     p->name, p->host, p->uri);

      }

      debug_printf("cups-browsed: Bonjour IDs: Service name: \"%s\", Service type: \"%s\", Domain: \"%s\"\n",
		   p->service_name, p->type, p->domain);

      recheck_timer ();
    }
    break;
  }

  /* All cached Avahi events are treated now */
  case AVAHI_BROWSER_ALL_FOR_NOW:
  case AVAHI_BROWSER_CACHE_EXHAUSTED:
    debug_printf("cups-browsed: Avahi Browser: %s\n",
		 event == AVAHI_BROWSER_CACHE_EXHAUSTED ?
		 "CACHE_EXHAUSTED" : "ALL_FOR_NOW");
    break;
  }

}

void avahi_browser_shutdown() {
  remote_printer_t *p;

  /* Remove all queues which we have set up based on Bonjour discovery*/
  for (p = (remote_printer_t *)cupsArrayFirst(remote_printers);
       p; p = (remote_printer_t *)cupsArrayNext(remote_printers)) {
    if (p->type && p->type[0]) {
      p->status = STATUS_DISAPPEARED;
      p->timeout = time(NULL) + TIMEOUT_IMMEDIATELY;
    }
  }
  handle_cups_queues(NULL);

  /* Free the data structures for Bonjour browsing */
  if (sb1) {
    avahi_service_browser_free(sb1);
    sb1 = NULL;
  }
  if (sb2) {
    avahi_service_browser_free(sb2);
    sb2 = NULL;
  }

  /* Switch on auto shutdown mode */
  if (autoshutdown_avahi) {
    autoshutdown = 1;
    debug_printf("cups-browsed: Avahi server disappeared, switching to auto shutdown mode ...\n");
    /* If there are no printers schedule the shutdown in autoshutdown_timeout
       seconds */
    if (!autoshutdown_exec_id &&
	cupsArrayCount(remote_printers) == 0) {
      debug_printf ("cups-browsed: We entered auto shutdown mode and no printers are there to make available, shutting down in %d sec...\n", autoshutdown_timeout);
      autoshutdown_exec_id =
	g_timeout_add_seconds (autoshutdown_timeout, autoshutdown_execute,
			       NULL);
    }
  }
}

void avahi_shutdown() {
  avahi_browser_shutdown();
  if (client) {
    avahi_client_free(client);
    client = NULL;
  }
  if (glib_poll) {
    avahi_glib_poll_free(glib_poll);
    glib_poll = NULL;
  }
}

static void client_callback(AvahiClient *c, AvahiClientState state, AVAHI_GCC_UNUSED void * userdata) {
  int error;

  assert(c);

  /* Called whenever the client or server state changes */
  switch (state) {

  /* avahi-daemon available */
  case AVAHI_CLIENT_S_REGISTERING:
  case AVAHI_CLIENT_S_RUNNING:
  case AVAHI_CLIENT_S_COLLISION:

    debug_printf("cups-browsed: Avahi server connection got available, setting up service browsers.\n");

    /* Create the service browsers */
    if (!sb1)
      if (!(sb1 =
	    avahi_service_browser_new(c, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC,
				      "_ipp._tcp", NULL, 0, browse_callback,
				      c))) {
	debug_printf("cups-browsed: ERROR: Failed to create service browser for IPP: %s\n",
		     avahi_strerror(avahi_client_errno(c)));
      }
    if (!sb2)
      if (!(sb2 =
	    avahi_service_browser_new(c, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC,
				      "_ipps._tcp", NULL, 0, browse_callback,
				      c))) {
	debug_printf("cups-browsed: ERROR: Failed to create service browser for IPPS: %s\n",
		     avahi_strerror(avahi_client_errno(c)));
      }

    /* switch off auto shutdown mode */
    if (autoshutdown_avahi) {
      autoshutdown = 0;
      debug_printf("cups-browsed: Avahi server available, switching to permanent mode ...\n");
      /* If there is still an active auto shutdown timer, kill it */
      if (autoshutdown_exec_id > 0) {
	debug_printf ("cups-browsed: We have left auto shutdown mode, killing auto shutdown timer.\n");
	g_source_remove(autoshutdown_exec_id);
	autoshutdown_exec_id = 0;
      }
    }

    break;

  /* Avahi client error */
  case AVAHI_CLIENT_FAILURE:

    if (avahi_client_errno(c) == AVAHI_ERR_DISCONNECTED) {
      debug_printf("cups-browsed: Avahi server disappeared, shutting down service browsers, removing Bonjour-discovered print queues.\n");
      avahi_browser_shutdown();
      /* Renewing client */
      avahi_client_free(client);
      client = avahi_client_new(avahi_glib_poll_get(glib_poll),
				AVAHI_CLIENT_NO_FAIL,
				client_callback, NULL, &error);
      if (!client) {
	debug_printf("cups-browsed: ERROR: Failed to create client: %s\n",
		     avahi_strerror(error));
	BrowseRemoteProtocols &= ~BROWSE_DNSSD;
	avahi_shutdown();
      }
    } else {
      debug_printf("cups-browsed: ERROR: Avahi server connection failure: %s\n",
		   avahi_strerror(avahi_client_errno(c)));
      g_main_loop_quit(gmainloop);
    }
    break;

  default:
    break;
  }
}

void avahi_init() {
  int error;

  if (BrowseRemoteProtocols & BROWSE_DNSSD) {
    /* Allocate main loop object */
    if (!glib_poll)
      if (!(glib_poll = avahi_glib_poll_new(NULL, G_PRIORITY_DEFAULT))) {
	debug_printf("cups-browsed: ERROR: Failed to create glib poll object.\n");
	goto avahi_init_fail;
      }

    /* Allocate a new client */
    if (!client)
      client = avahi_client_new(avahi_glib_poll_get(glib_poll),
				AVAHI_CLIENT_NO_FAIL,
				client_callback, NULL, &error);

    /* Check wether creating the client object succeeded */
    if (!client) {
      debug_printf("cups-browsed: ERROR: Failed to create client: %s\n",
		   avahi_strerror(error));
      goto avahi_init_fail;
    }

    return;

  avahi_init_fail:
    BrowseRemoteProtocols &= ~BROWSE_DNSSD;
    avahi_shutdown();
  }
}
#endif /* HAVE_AVAHI */

/*
 * A CUPS printer has been discovered via CUPS Browsing
 * or with BrowsePoll
 */
void
found_cups_printer (const char *remote_host, const char *uri,
		    const char *info)
{
  char scheme[32];
  char username[64];
  char host[HTTP_MAX_HOST];
  char resource[HTTP_MAX_URI];
  int port;
  netif_t *iface;
  char local_resource[HTTP_MAX_URI];
  char *c;
  remote_printer_t *printer;

  memset(scheme, 0, sizeof(scheme));
  memset(username, 0, sizeof(username));
  memset(host, 0, sizeof(host));
  memset(resource, 0, sizeof(resource));
  memset(local_resource, 0, sizeof(local_resource));

  httpSeparateURI (HTTP_URI_CODING_ALL, uri,
		   scheme, sizeof(scheme) - 1,
		   username, sizeof(username) - 1,
		   host, sizeof(host) - 1,
		   &port,
		   resource, sizeof(resource)- 1);

  /* Check this isn't one of our own broadcasts */
  for (iface = cupsArrayFirst (netifs);
       iface;
       iface = cupsArrayNext (netifs))
    if (!strcasecmp (host, iface->address))
      break;
  if (iface) {
    debug_printf("cups-browsed: ignoring own broadcast on %s\n",
		 iface->address);
    return;
  }

  if (strncasecmp (resource, "/printers/", 10) &&
      strncasecmp (resource, "/classes/", 9)) {
    debug_printf("cups-browsed: don't understand URI: %s\n", uri);
    return;
  }

  strncpy (local_resource, resource + 1, sizeof (local_resource) - 1);
  local_resource[sizeof (local_resource) - 1] = '\0';
  c = strchr (local_resource, '?');
  if (c)
    *c = '\0';

  debug_printf("cups-browsed: browsed queue name is %s\n",
	       local_resource + 9);

  printer = generate_local_queue(host, port, local_resource,
				 info ? info : "",
				 "", "", NULL);

  if (printer) {
    if (printer->status == STATUS_TO_BE_CREATED)
      printer->status = STATUS_BROWSE_PACKET_RECEIVED;
    else {
      printer->status = STATUS_DISAPPEARED;
      printer->timeout = time(NULL) + BrowseTimeout;
    }
  }
}

static gboolean
allowed (struct sockaddr *srcaddr)
{
  allow_t *allow;
  if (browseallow_all || cupsArrayCount(browseallow) == 0) {
    /* "BrowseAllow All", or no "BrowseAllow" line, so allow all servers */
    return TRUE;
  }
  for (allow = cupsArrayFirst (browseallow);
       allow;
       allow = cupsArrayNext (browseallow)) {
    switch (allow->type) {
    case ALLOW_INVALID:
      break;

    case ALLOW_IP:
      switch (srcaddr->sa_family) {
      case AF_INET:
	if (((struct sockaddr_in *) srcaddr)->sin_addr.s_addr ==
	    allow->addr.ipv4.sin_addr.s_addr)
	  return TRUE;
	break;

      case AF_INET6:
	if (!memcmp (&((struct sockaddr_in6 *) srcaddr)->sin6_addr,
		     &allow->addr.ipv6.sin6_addr,
		     sizeof (allow->addr.ipv6.sin6_addr)))
	  return TRUE;
	break;
      }
      break;

    case ALLOW_NET:
      switch (srcaddr->sa_family) {
	struct sockaddr_in6 *src6addr;

      case AF_INET:
	if ((((struct sockaddr_in *) srcaddr)->sin_addr.s_addr &
	     allow->mask.ipv4.sin_addr.s_addr) ==
	    allow->addr.ipv4.sin_addr.s_addr)
	  return TRUE;
	break;

      case AF_INET6:
	src6addr = (struct sockaddr_in6 *) srcaddr;
	if (((src6addr->sin6_addr.s6_addr[0] &
	      allow->mask.ipv6.sin6_addr.s6_addr[0]) ==
	     allow->addr.ipv6.sin6_addr.s6_addr[0]) &&
	    ((src6addr->sin6_addr.s6_addr[1] &
	      allow->mask.ipv6.sin6_addr.s6_addr[1]) ==
	     allow->addr.ipv6.sin6_addr.s6_addr[1]) &&
	    ((src6addr->sin6_addr.s6_addr[2] &
	      allow->mask.ipv6.sin6_addr.s6_addr[2]) ==
	     allow->addr.ipv6.sin6_addr.s6_addr[2]) &&
	    ((src6addr->sin6_addr.s6_addr[3] &
	      allow->mask.ipv6.sin6_addr.s6_addr[3]) ==
	     allow->addr.ipv6.sin6_addr.s6_addr[3]))
	  return TRUE;
	break;
      }
    }
  }

  return FALSE;
}

gboolean
process_browse_data (GIOChannel *source,
		     GIOCondition condition,
		     gpointer data)
{
  char packet[2048];
  http_addr_t srcaddr;
  socklen_t srclen;
  ssize_t got;
  unsigned int type;
  unsigned int state;
  char remote_host[256];
  char uri[1024];
  char info[1024];
  char *c = NULL, *end = NULL;

  memset(packet, 0, sizeof(packet));
  memset(remote_host, 0, sizeof(remote_host));
  memset(uri, 0, sizeof(uri));
  memset(info, 0, sizeof(info));

  srclen = sizeof (srcaddr);
  got = recvfrom (browsesocket, packet, sizeof (packet) - 1, 0,
		  &srcaddr.addr, &srclen);
  if (got == -1) {
    debug_printf ("cupsd-browsed: error receiving browse packet: %s\n",
		  strerror (errno));
    /* Remove this I/O source */
    return FALSE;
  }

  packet[got] = '\0';
  httpAddrString (&srcaddr, remote_host, sizeof (remote_host) - 1);

  /* Check this packet is allowed */
  if (!allowed ((struct sockaddr *) &srcaddr)) {
    debug_printf("cups-browsed: browse packet from %s disallowed\n",
		 remote_host);
    return TRUE;
  }

  debug_printf("cups-browsed: browse packet received from %s\n",
	       remote_host);

  if (sscanf (packet, "%x%x%1023s", &type, &state, uri) < 3) {
    debug_printf("cups-browsed: incorrect browse packet format\n");
    return TRUE;
  }

  info[0] = '\0';

  /* do not read OOB */
  end = packet + sizeof(packet);
  c = strchr (packet, '\"');
  if (c >= end)
     return TRUE;

  if (c) {
    /* Skip location field */
    for (c++; c < end && *c != '\"'; c++)
      ;

    if (c >= end)
       return TRUE;

    if (*c == '\"') {
      for (c++; c < end && isspace(*c); c++)
	;
    }

    if (c >= end)
      return TRUE;

    /* Is there an info field? */
    if (*c == '\"') {
      int i;
      c++;
      for (i = 0;
	   i < sizeof (info) - 1 && *c != '\"' && c < end;
	   i++, c++)
	info[i] = *c;
      info[i] = '\0';
    }
  }
  if (c >= end)
    return TRUE;

  if (!(type & CUPS_PRINTER_DELETE))
    found_cups_printer (remote_host, uri, info);

  recheck_timer ();

  /* Don't remove this I/O source */
  return TRUE;
}

static gboolean
update_netifs (gpointer data)
{
  struct ifaddrs *ifaddr, *ifa;
  netif_t *iface;

  update_netifs_sourceid = -1;
  if (getifaddrs (&ifaddr) == -1) {
    debug_printf("cups-browsed: unable to get interface addresses: %s\n",
		 strerror (errno));
    return FALSE;
  }

  while ((iface = cupsArrayFirst (netifs)) != NULL) {
    cupsArrayRemove (netifs, iface);
    free (iface->address);
    free (iface);
  }

  for (ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
    netif_t *iface;

    if (ifa->ifa_addr == NULL)
      continue;

    if (ifa->ifa_broadaddr == NULL)
      continue;

    if (ifa->ifa_flags & IFF_LOOPBACK)
      continue;

    if (!(ifa->ifa_flags & IFF_BROADCAST))
      continue;

    iface = malloc (sizeof (netif_t));
    if (iface == NULL) {
      debug_printf ("cups-browsed: malloc failure\n");
      exit (1);
    }

    iface->address = malloc (HTTP_MAX_HOST);
    if (iface->address == NULL) {
      free (iface);
      debug_printf ("cups-browsed: malloc failure\n");
      exit (1);
    }

    iface->address[0] = '\0';
    switch (ifa->ifa_addr->sa_family) {
    case AF_INET:
      getnameinfo (ifa->ifa_addr, sizeof (struct sockaddr_in),
		   iface->address, HTTP_MAX_HOST,
		   NULL, 0, NI_NUMERICHOST);
      memcpy (&iface->broadcast, ifa->ifa_broadaddr,
	      sizeof (struct sockaddr_in));
      iface->broadcast.ipv4.sin_port = htons (BrowsePort);
      break;

    case AF_INET6:
      if (IN6_IS_ADDR_LINKLOCAL (&((struct sockaddr_in6 *)(ifa->ifa_addr))
				 ->sin6_addr))
	break;

      getnameinfo (ifa->ifa_addr, sizeof (struct sockaddr_in6),
		   iface->address, HTTP_MAX_HOST, NULL, 0, NI_NUMERICHOST);
      memcpy (&iface->broadcast, ifa->ifa_broadaddr,
	      sizeof (struct sockaddr_in6));
      iface->broadcast.ipv6.sin6_port = htons (BrowsePort);
      break;
    }

    if (iface->address[0]) {
      cupsArrayAdd (netifs, iface);
      debug_printf("cups-browsed: network interface at %s\n", iface->address);
    } else {
      free (iface->address);
      free (iface);
    }
  }

  freeifaddrs (ifaddr);

  /* If run as a timeout, don't run it again. */
  return FALSE;
}

static void
broadcast_browse_packets (gpointer data, gpointer user_data)
{
  browse_data_t *bdata = data;
  netif_t *browse;
  char packet[2048];
  char uri[HTTP_MAX_URI];
  char scheme[32];
  char username[64];
  char host[HTTP_MAX_HOST];
  int port;
  char resource[HTTP_MAX_URI];

  for (browse = (netif_t *)cupsArrayFirst (netifs);
       browse != NULL;
       browse = (netif_t *)cupsArrayNext (netifs)) {
    /* Replace 'localhost' with our IP address on this interface */
    httpSeparateURI(HTTP_URI_CODING_ALL, bdata->uri,
		    scheme, sizeof(scheme),
		    username, sizeof(username),
		    host, sizeof(host),
		    &port,
		    resource, sizeof(resource));
    httpAssembleURI(HTTP_URI_CODING_ALL, uri, sizeof (uri),
		    scheme, username, browse->address, port, resource);

    if (snprintf (packet, sizeof (packet),
		  "%x "     /* type */
		  "%x "     /* state */
		  "%s "     /* uri */
		  "\"%s\" " /* location */
		  "\"%s\" " /* info */
		  "\"%s\" " /* make-and-model */
		  "lease-duration=%d" /* BrowseTimeout */
		  "%s%s" /* other browse options */
		  "\n",
		  bdata->type,
		  bdata->state,
		  uri,
		  bdata->location,
		  bdata->info,
		  bdata->make_model,
		  BrowseTimeout,
		  bdata->browse_options ? " " : "",
		  bdata->browse_options ? bdata->browse_options : "")
	>= sizeof (packet)) {
      debug_printf ("cups-browsed: oversize packet not sent\n");
      continue;
    }

    debug_printf("cups-browsed: packet to send:\n%s", packet);

    int err = sendto (browsesocket, packet,
		      strlen (packet), 0,
		      &browse->broadcast.addr,
		      httpAddrLength (&browse->broadcast));
    if (err == -1)
      debug_printf("cupsd-browsed: sendto returned %d: %s\n",
		   err, strerror (errno));
  }
}

gboolean
send_browse_data (gpointer data)
{
  update_netifs (NULL);
  res_init ();
  update_local_printers ();
  g_list_foreach (browse_data, broadcast_browse_packets, NULL);
  g_timeout_add_seconds (BrowseInterval, send_browse_data, NULL);

  /* Stop this timeout handler, we called a new one */
  return FALSE;
}

static browsepoll_printer_t *
new_browsepoll_printer (const char *uri_supported,
			const char *info)
{
  browsepoll_printer_t *printer = g_malloc (sizeof (browsepoll_printer_t));
  printer->uri_supported = g_strdup (uri_supported);
  printer->info = g_strdup (info);
  return printer;
}

static void
browsepoll_printer_free (gpointer data)
{
  browsepoll_printer_t *printer = data;
  free (printer->uri_supported);
  free (printer->info);
  free (printer);
}

static void
browse_poll_get_printers (browsepoll_t *context, http_t *conn)
{
  static const char * const rattrs[] = { "printer-uri-supported",
					 "printer-info"};
  ipp_t *request, *response = NULL;
  ipp_attribute_t *attr;
  GList *printers = NULL;

  debug_printf ("cups-browsed [BrowsePoll %s:%d]: CUPS-Get-Printers\n",
		context->server, context->port);

  request = ippNewRequest(CUPS_GET_PRINTERS);
  if (context->major > 0) {
    debug_printf("cups-browsed [BrowsePoll %s:%d]: setting IPP version %d.%d\n",
		 context->server, context->port, context->major,
		 context->minor);
    ippSetVersion (request, context->major, context->minor);
  }

  ippAddStrings (request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD,
		 "requested-attributes", sizeof (rattrs) / sizeof (rattrs[0]),
		 NULL,
		 rattrs);

  /* Ask the server to exclude printers that are remote or not shared,
     or implicit classes. */
  ippAddInteger (request, IPP_TAG_OPERATION, IPP_TAG_ENUM,
		 "printer-type-mask",
		 CUPS_PRINTER_REMOTE | CUPS_PRINTER_IMPLICIT |
		 CUPS_PRINTER_NOT_SHARED);
  ippAddInteger (request, IPP_TAG_OPERATION, IPP_TAG_ENUM,
		 "printer-type", 0);

  ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_NAME,
		"requesting-user-name", NULL, cupsUser ());

  response = cupsDoRequest(conn, request, "/");
  if (cupsLastError() > IPP_OK_CONFLICT) {
    debug_printf("cups-browsed [BrowsePoll %s:%d]: failed: %s\n",
		 context->server, context->port, cupsLastErrorString ());
    goto fail;
  }

  for (attr = ippFirstAttribute(response); attr;
       attr = ippNextAttribute(response)) {
    browsepoll_printer_t *printer;
    const char *uri, *info;

    while (attr && ippGetGroupTag(attr) != IPP_TAG_PRINTER)
      attr = ippNextAttribute(response);

    if (!attr)
      break;

    uri = NULL;
    info = NULL;
    while (attr && ippGetGroupTag(attr) == IPP_TAG_PRINTER) {

      if (!strcasecmp (ippGetName(attr), "printer-uri-supported") &&
	  ippGetValueTag(attr) == IPP_TAG_URI)
	uri = ippGetString(attr, 0, NULL);
      else if (!strcasecmp (ippGetName(attr), "printer-info") &&
	       ippGetValueTag(attr) == IPP_TAG_TEXT)
	info = ippGetString(attr, 0, NULL);

      attr = ippNextAttribute(response);
    }

    if (uri) {
      found_cups_printer (context->server, uri, info);
      printer = new_browsepoll_printer (uri, info);
      printers = g_list_insert (printers, printer, 0);
    }

    if (!attr)
      break;
  }

  g_list_free_full (context->printers, browsepoll_printer_free);
  context->printers = printers;
  recheck_timer ();

fail:
  if (response)
    ippDelete(response);
}

static void
browse_poll_create_subscription (browsepoll_t *context, http_t *conn)
{
  static const char * const events[] = { "printer-added",
					 "printer-changed",
					 "printer-config-changed",
					 "printer-modified",
					 "printer-deleted",
					 "printer-state-changed" };
  ipp_t *request, *response = NULL;
  ipp_attribute_t *attr;

  debug_printf ("cups-browsed [BrowsePoll %s:%d]: IPP-Create-Subscription\n",
		context->server, context->port);

  request = ippNewRequest(IPP_CREATE_PRINTER_SUBSCRIPTION);
  if (context->major > 0) {
    debug_printf("cups-browsed [BrowsePoll %s:%d]: setting IPP version %d.%d\n",
		 context->server, context->port, context->major,
		 context->minor);
    ippSetVersion (request, context->major, context->minor);
  }

  ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_URI,
		"printer-uri", NULL, "/");
  ippAddString (request, IPP_TAG_SUBSCRIPTION, IPP_TAG_KEYWORD,
		"notify-pull-method", NULL, "ippget");
  ippAddString (request, IPP_TAG_SUBSCRIPTION, IPP_TAG_CHARSET,
		"notify-charset", NULL, "utf-8");
  ippAddString (request, IPP_TAG_SUBSCRIPTION, IPP_TAG_NAME,
		"requesting-user-name", NULL, cupsUser ());
  ippAddStrings (request, IPP_TAG_SUBSCRIPTION, IPP_TAG_KEYWORD,
		 "notify-events", sizeof (events) / sizeof (events[0]),
		 NULL, events);
  ippAddInteger (request, IPP_TAG_SUBSCRIPTION, IPP_TAG_INTEGER,
		 "notify-time-interval", BrowseInterval);

  response = cupsDoRequest (conn, request, "/");
  if (!response || ippGetStatusCode (response) > IPP_OK_CONFLICT) {
    debug_printf("cupsd-browsed [BrowsePoll %s:%d]: failed: %s\n",
		 context->server, context->port, cupsLastErrorString ());
    context->subscription_id = -1;
    context->can_subscribe = FALSE;
    goto fail;
  }

  for (attr = ippFirstAttribute(response); attr;
       attr = ippNextAttribute(response)) {
    if (ippGetGroupTag (attr) == IPP_TAG_SUBSCRIPTION) {
      if (ippGetValueTag (attr) == IPP_TAG_INTEGER &&
	  !strcasecmp (ippGetName (attr), "notify-subscription-id")) {
	context->subscription_id = ippGetInteger (attr, 0);
	debug_printf("cups-browsed [BrowsePoll %s:%d]: subscription ID=%d\n",
		     context->server, context->port, context->subscription_id);
	break;
      }
    }
  }

  if (!attr) {
    debug_printf("cups-browsed [BrowsePoll %s:%d]: no ID returned\n",
		 context->server, context->port);
    context->subscription_id = -1;
    context->can_subscribe = FALSE;
  }

fail:
  if (response)
    ippDelete(response);
}

static void
browse_poll_cancel_subscription (browsepoll_t *context)
{
  ipp_t *request, *response = NULL;
  http_t *conn = httpConnectEncrypt (context->server, context->port,
				     HTTP_ENCRYPT_IF_REQUESTED);

  if (conn == NULL) {
    debug_printf("cups-browsed [BrowsePoll %s:%d]: connection failure "
		 "attempting to cancel\n", context->server, context->port);
    return;
  }

  debug_printf ("cups-browsed [BrowsePoll %s:%d]: IPP-Cancel-Subscription\n",
		context->server, context->port);

  request = ippNewRequest(IPP_CANCEL_SUBSCRIPTION);
  if (context->major > 0) {
    debug_printf("cups-browsed [BrowsePoll %s:%d]: setting IPP version %d.%d\n",
		 context->server, context->port, context->major,
		 context->minor);
    ippSetVersion (request, context->major, context->minor);
  }

  ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_URI,
		"printer-uri", NULL, "/");
  ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_NAME,
		"requesting-user-name", NULL, cupsUser ());
  ippAddInteger (request, IPP_TAG_OPERATION, IPP_TAG_INTEGER,
		 "notify-subscription-id", context->subscription_id);

  response = cupsDoRequest (conn, request, "/");
  if (!response || ippGetStatusCode (response) > IPP_OK_CONFLICT)
    debug_printf("cupsd-browsed [BrowsePoll %s:%d]: failed: %s\n",
		 context->server, context->port, cupsLastErrorString ());

  if (response)
    ippDelete(response);

  httpClose (conn);
}

static gboolean
browse_poll_get_notifications (browsepoll_t *context, http_t *conn)
{
  ipp_t *request, *response = NULL;
  ipp_status_t status;
  gboolean get_printers = FALSE;

  debug_printf ("cups-browsed [BrowsePoll %s:%d]: IPP-Get-Notifications\n",
		context->server, context->port);

  request = ippNewRequest(IPP_GET_NOTIFICATIONS);
  if (context->major > 0) {
    debug_printf("cups-browsed [BrowsePoll %s:%d]: setting IPP version %d.%d\n",
		 context->server, context->port, context->major,
		 context->minor);
    ippSetVersion (request, context->major, context->minor);
  }

  ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_URI,
		"printer-uri", NULL, "/");
  ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_NAME,
		"requesting-user-name", NULL, cupsUser ());
  ippAddInteger (request, IPP_TAG_OPERATION, IPP_TAG_INTEGER,
		 "notify-subscription-ids", context->subscription_id);
  ippAddInteger (request, IPP_TAG_OPERATION, IPP_TAG_INTEGER,
		 "notify-sequence-numbers", context->sequence_number + 1);

  response = cupsDoRequest (conn, request, "/");
  if (!response)
    status = cupsLastError ();
  else
    status = ippGetStatusCode (response);

  if (status == IPP_NOT_FOUND) {
    /* Subscription lease has expired. */
    debug_printf ("cups-browsed [BrowsePoll %s:%d]: Lease expired\n",
		  context->server, context->port);
    browse_poll_create_subscription (context, conn);
    get_printers = TRUE;
  } else if (status > IPP_OK_CONFLICT) {
    debug_printf("cupsd-browsed [BrowsePoll %s:%d]: failed: %s\n",
		 context->server, context->port, cupsLastErrorString ());
    context->can_subscribe = FALSE;
    browse_poll_cancel_subscription (context);
    context->subscription_id = -1;
    context->sequence_number = 0;
    get_printers = TRUE;
  }

  if (!get_printers) {
    ipp_attribute_t *attr;
    gboolean seen_event = FALSE;
    int last_seq = context->sequence_number;
    assert (response != NULL);
    for (attr = ippFirstAttribute(response); attr;
	 attr = ippNextAttribute(response))
      if (ippGetGroupTag (attr) == IPP_TAG_EVENT_NOTIFICATION) {
	/* There is a printer-* event here. */
	seen_event = TRUE;

	if (!strcmp (ippGetName (attr), "notify-sequence-number") &&
	    ippGetValueTag (attr) == IPP_TAG_INTEGER)
	  last_seq = ippGetInteger (attr, 0);
      }

    if (seen_event) {
      debug_printf("cups-browsed [BrowsePoll %s:%d]: printer-* event\n",
		   context->server, context->port);
      context->sequence_number = last_seq;
      get_printers = TRUE;
    } else
      debug_printf("cups-browsed [BrowsePoll %s:%d]: no events\n",
		   context->server, context->port);
  }

  if (response)
    ippDelete (response);

  return get_printers;
}

static void
browsepoll_printer_keepalive (gpointer data, gpointer user_data)
{
  browsepoll_printer_t *printer = data;
  const char *server = user_data;
  found_cups_printer (server, printer->uri_supported, printer->info);
}

gboolean
browse_poll (gpointer data)
{
  browsepoll_t *context = data;
  http_t *conn = NULL;
  gboolean get_printers = FALSE;

  debug_printf ("cups-browsed: browse polling %s:%d\n",
		context->server, context->port);

  res_init ();

  conn = httpConnectEncrypt (context->server, context->port,
			     HTTP_ENCRYPT_IF_REQUESTED);
  if (conn == NULL) {
    debug_printf("cups-browsed [BrowsePoll %s:%d]: failed to connect\n",
		 context->server, context->port);
    goto fail;
  }

  if (context->can_subscribe) {
    if (context->subscription_id == -1) {
      /* The first time this callback is run we need to create the IPP
       * subscription to watch to printer-* events. */
      browse_poll_create_subscription (context, conn);
      get_printers = TRUE;
    } else
      /* On subsequent runs, check for notifications using our
       * subscription. */
      get_printers = browse_poll_get_notifications (context, conn);
  }
  else
    get_printers = TRUE;

  update_local_printers ();
  inhibit_local_printers_update = TRUE;
  if (get_printers)
    browse_poll_get_printers (context, conn);
  else
    g_list_foreach (context->printers, browsepoll_printer_keepalive,
		    context->server);

  inhibit_local_printers_update = FALSE;

fail:

  if (conn)
    httpClose (conn);

  /* Call a new timeout handler so that we run again */
  g_timeout_add_seconds (BrowseInterval, browse_poll, data);

  /* Stop this timeout handler, we called a new one */
  return FALSE;
}

int
compare_pointers (void *a, void *b, void *data)
{
  if (a < b)
    return -1;
  if (a > b)
    return 1;
  return 0;
}

int compare_remote_printers (remote_printer_t *a, remote_printer_t *b) {
  return strcasecmp(a->name, b->name);
}

static void
sigterm_handler(int sig) {
  (void)sig;    /* remove compiler warnings... */

  /* Flag that we should stop and return... */
  g_main_loop_quit(gmainloop);
  debug_printf("cups-browsed: Caught signal %d, shutting down ...\n", sig);
}

static void
sigusr1_handler(int sig) {
  (void)sig;    /* remove compiler warnings... */

  /* Turn off auto shutdown mode... */
  autoshutdown = 0;
  debug_printf("cups-browsed: Caught signal %d, switching to permanent mode ...\n", sig);
  /* If there is still an active auto shutdown timer, kill it */
  if (autoshutdown_exec_id > 0) {
    debug_printf ("cups-browsed: We have left auto shutdown mode, killing auto shutdown timer.\n");
    g_source_remove(autoshutdown_exec_id);
    autoshutdown_exec_id = 0;
  }
}

static void
sigusr2_handler(int sig) {
  (void)sig;    /* remove compiler warnings... */

  /* Turn on auto shutdown mode... */
  autoshutdown = 1;
  debug_printf("cups-browsed: Caught signal %d, switching to auto shutdown mode ...\n", sig);
  /* If there are no printers schedule the shutdown in autoshutdown_timeout
     seconds */
  if (!autoshutdown_exec_id &&
      cupsArrayCount(remote_printers) == 0) {
    debug_printf ("cups-browsed: We entered auto shutdown mode and no printers are there to make available, shutting down in %d sec...\n", autoshutdown_timeout);
    autoshutdown_exec_id =
      g_timeout_add_seconds (autoshutdown_timeout, autoshutdown_execute,
			       NULL);
  }
}

static int
read_browseallow_value (const char *value)
{
  char *p;
  struct in_addr addr;
  allow_t *allow;

  if (!strcasecmp (value, "all")) {
    browseallow_all = TRUE;
    return 0;
  }
  
  allow = calloc (1, sizeof (allow_t));
  if (value == NULL)
    goto fail;
  p = strchr (value, '/');
  if (p) {
    char *s = strdup (value);
    s[p - value] = '\0';

    if (!inet_aton (s, &addr)) {
      free (s);
      goto fail;
    }

    free (s);
    allow->type = ALLOW_NET;
    allow->addr.ipv4.sin_addr.s_addr = addr.s_addr;

    p++;
    if (strchr (p, '.')) {
      if (inet_aton (p, &addr))
	allow->mask.ipv4.sin_addr.s_addr = addr.s_addr;
      else
	goto fail;
    } else {
      char *endptr;
      unsigned long bits = strtoul (p, &endptr, 10);
      if (p == endptr)
	goto fail;

      if (bits > 32)
	goto fail;

      allow->mask.ipv4.sin_addr.s_addr = htonl (((0xffffffff << (32 - bits)) &
						 0xffffffff));
    }
  } else if (inet_aton (value, &addr)) {
    allow->type = ALLOW_IP;
    allow->addr.ipv4.sin_addr.s_addr = addr.s_addr;
  } else
    goto fail;

  cupsArrayAdd (browseallow, allow);
  return 0;

fail:
  allow->type = ALLOW_INVALID;
  cupsArrayAdd (browseallow, allow);
  return 1;
}

void
read_configuration (const char *filename)
{
  cups_file_t *fp;
  int linenum;
  char line[HTTP_MAX_BUFFER];
  char *value;
  const char *delim = " \t,";

  if (!filename)
    filename = CUPS_SERVERROOT "/cups-browsed.conf";

  if ((fp = cupsFileOpen(filename, "r")) == NULL) {
    debug_printf("cups-browsed: unable to open configuration file; "
		 "using defaults\n");
    return;
  }

  while (cupsFileGetConf(fp, line, sizeof(line), &value, &linenum)) {
    debug_printf("cups-browsed: Reading config: %s %s\n", line, value);
    if ((!strcasecmp(line, "BrowseProtocols") ||
	 !strcasecmp(line, "BrowseLocalProtocols") ||
	 !strcasecmp(line, "BrowseRemoteProtocols")) && value) {
      int protocols = 0;
      char *p, *saveptr;
      p = strtok_r (value, delim, &saveptr);
      while (p) {
	if (!strcasecmp(p, "dnssd"))
	  protocols |= BROWSE_DNSSD;
	else if (!strcasecmp(p, "cups"))
	  protocols |= BROWSE_CUPS;
	else if (strcasecmp(p, "none"))
	  debug_printf("cups-browsed: Unknown protocol '%s'\n", p);

	p = strtok_r (NULL, delim, &saveptr);
      }

      if (!strcasecmp(line, "BrowseLocalProtocols"))
	BrowseLocalProtocols = protocols;
      else if (!strcasecmp(line, "BrowseRemoteProtocols"))
	BrowseRemoteProtocols = protocols;
      else
	BrowseLocalProtocols = BrowseRemoteProtocols = protocols;
    } else if (!strcasecmp(line, "BrowsePoll") && value) {
      browsepoll_t **old = BrowsePoll;
      BrowsePoll = realloc (BrowsePoll,
			    (NumBrowsePoll + 1) *
			    sizeof (browsepoll_t));
      if (!BrowsePoll) {
	debug_printf("cups-browsed: unable to realloc: ignoring BrowsePoll line\n");
	BrowsePoll = old;
      } else {
	char *colon, *slash;
	browsepoll_t *b = g_malloc0 (sizeof (browsepoll_t));
	debug_printf("cups-browsed: Adding BrowsePoll server: %s\n", value);
	b->server = strdup (value);
	b->port = BrowsePort;
	b->can_subscribe = TRUE; /* first assume subscriptions work */
	b->subscription_id = -1;
	slash = strchr (b->server, '/');
	if (slash) {
	  *slash++ = '\0';
	  if (!strcasecmp (slash, "version=1.0")) {
	    b->major = 1;
	    b->minor = 0;
	  } else if (!strcasecmp (slash, "version=1.1")) {
	    b->major = 1;
	    b->minor = 1;
	  } else if (!strcasecmp (slash, "version=2.0")) {
	    b->major = 2;
	    b->minor = 0;
	  } else if (!strcasecmp (slash, "version=2.1")) {
	    b->major = 2;
	    b->minor = 1;
	  } else if (!strcasecmp (slash, "version=2.2")) {
	    b->major = 2;
	    b->minor = 2;
	  } else {
	    debug_printf ("ignoring unknown server option: %s\n", slash);
	  }
	} else
	  b->major = 0;

	colon = strchr (b->server, ':');
	if (colon) {
	  char *endptr;
	  unsigned long n;
	  *colon++ = '\0';
	  n = strtoul (colon, &endptr, 10);
	  if (endptr != colon && n < INT_MAX)
	    b->port = (int) n;
	}

	BrowsePoll[NumBrowsePoll++] = b;
      }
    } else if (!strcasecmp(line, "BrowseAllow")) {
      if (read_browseallow_value (value))
	debug_printf ("cups-browsed: BrowseAllow value \"%s\" not understood\n",
		      value);
    } else if (!strcasecmp(line, "DomainSocket") && value) {
      if (value[0] != '\0')
	DomainSocket = strdup(value);
    } else if (!strcasecmp(line, "CreateIPPPrinterQueues") && value) {
      if (!strcasecmp(value, "yes") || !strcasecmp(value, "true") ||
	  !strcasecmp(value, "on") || !strcasecmp(value, "1"))
	CreateIPPPrinterQueues = 1;
      else if (!strcasecmp(value, "no") || !strcasecmp(value, "false") ||
	  !strcasecmp(value, "off") || !strcasecmp(value, "0"))
	CreateIPPPrinterQueues = 0;
    } else if (!strcasecmp(line, "AutoShutdown") && value) {
      char *p, *saveptr;
      p = strtok_r (value, delim, &saveptr);
      while (p) {
	if (!strcasecmp(p, "On") || !strcasecmp(p, "Yes") ||
	    !strcasecmp(p, "True") || !strcasecmp(p, "1")) {
	  autoshutdown = 1;
	  debug_printf("cups-browsed: Turning on auto shutdown mode.\n");
	} else if (!strcasecmp(p, "Off") || !strcasecmp(p, "No") ||
	    !strcasecmp(p, "False") || !strcasecmp(p, "0")) {
	  autoshutdown = 0;
	  debug_printf("cups-browsed: Turning off auto shutdown mode (permanent mode).\n");
	} else if (!strcasecmp(p, "avahi")) {
	  autoshutdown_avahi = 1;
	  debug_printf("cups-browsed: Turning on auto shutdown control by appearing and disappearing of the Avahi server.\n");
	} else if (strcasecmp(p, "none"))
	  debug_printf("cups-browsed: Unknown mode '%s'\n", p);
	p = strtok_r (NULL, delim, &saveptr);
      }
    } else if (!strcasecmp(line, "AutoShutdownTimeout") && value) {
      int t = atoi(value);
      if (t >= 0) {
	autoshutdown_timeout = t;
	debug_printf("cups-browsed: Set auto shutdown timeout to %d sec.\n",
		     t);
      } else
	debug_printf("cups-browsed: Invalid auto shutdown timeout value: %d\n",
		     t);
    }
  }

  cupsFileClose(fp);
}

static void
defer_update_netifs (void)
{
  if (update_netifs_sourceid != -1)
    g_source_remove (update_netifs_sourceid);

  update_netifs_sourceid = g_timeout_add_seconds (10, update_netifs, NULL);
}

static void
nm_properties_changed (GDBusProxy *proxy,
		       GVariant *changed_properties,
		       const gchar *const *invalidated_properties,
		       gpointer user_data)
{
  GVariantIter *iter;
  const gchar *key;
  GVariant *value;
  g_variant_get (changed_properties, "a{sv}", &iter);
  while (g_variant_iter_loop (iter, "{&sv}", &key, &value)) {
    if (!strcmp (key, "ActiveConnections")) {
      debug_printf ("cups-browsed: NetworkManager ActiveConnections changed\n");
      defer_update_netifs ();
      break;
    }
  }

  g_variant_iter_free (iter);
}

static void
find_previous_queue (gpointer key,
		     gpointer value,
		     gpointer user_data)
{
  const char *name = key;
  const local_printer_t *printer = value;
  remote_printer_t *p;
  if (printer->cups_browsed_controlled) {
    /* Queue found, add to our list */
    p = create_local_queue (name,
			    printer->device_uri,
			    "", "", "", "", NULL, NULL, 1);
    if (p) {
      /* Mark as unconfirmed, if no Avahi report of this queue appears
	 in a certain time frame, we will remove the queue */
      p->status = STATUS_UNCONFIRMED;

      if (BrowseRemoteProtocols & BROWSE_CUPS)
	p->timeout = time(NULL) + BrowseTimeout;
      else
	p->timeout = time(NULL) + TIMEOUT_CONFIRM;

      p->duplicate = 0;
      debug_printf("cups-browsed: Found CUPS queue %s (URI: %s) from previous session.\n",
		   p->name, p->uri);
    } else {
      debug_printf("cups-browsed: ERROR: Unable to allocate memory.\n");
      exit(1);
    }
  }
}

int main(int argc, char*argv[]) {
  int ret = 1;
  http_t *http;
  int i;
  const char *val;
  remote_printer_t *p;
  GDBusProxy *proxy = NULL;

  /* Turn on debug mode if requested */
  if (argc >= 2)
    for (i = 1; i < argc; i++)
      if (!strcasecmp(argv[i], "--debug") || !strcasecmp(argv[i], "-d") ||
	  !strncasecmp(argv[i], "-v", 2)) {
	debug = 1;
	debug_printf("cups-browsed: Reading command line: %s\n", argv[i]);
      }

  /* Initialise the browseallow array */
  browseallow = cupsArrayNew(compare_pointers, NULL);

  /* Read in cups-browsed.conf */
  read_configuration (NULL);

  /* Parse command line options after reading the config file to override
     config file settings */
  if (argc >= 2) {
    for (i = 1; i < argc; i++)
      if (!strncasecmp(argv[i], "--autoshutdown-timeout", 22)) {
	debug_printf("cups-browsed: Reading command line: %s\n", argv[i]);
	if (argv[i][22] == '=' && argv[i][23])
	  val = argv[i] + 23;
	else if (!argv[i][22] && i < argc -1) {
	  i++;
	  debug_printf("cups-browsed: Reading command line: %s\n", argv[i]);
	  val = argv[i];
	} else {
	  fprintf(stderr, "cups-browsed: Expected auto shutdown timeout setting after \"--autoshutdown-timeout\" option.\n");
	  exit(1);
	}
	int t = atoi(val);
	if (t >= 0) {
	  autoshutdown_timeout = t;
	  debug_printf("cups-browsed: Set auto shutdown timeout to %d sec.\n",
		       t);
	} else {
	  debug_printf("cups-browsed: Invalid auto shutdown timeout value: %d\n",
		       t);
	  exit(1);
	}
      } else if (!strncasecmp(argv[i], "--autoshutdown", 14)) {
	debug_printf("cups-browsed: Reading command line: %s\n", argv[i]);
	if (argv[i][14] == '=' && argv[i][15])
	  val = argv[i] + 15;
	else if (!argv[i][14] && i < argc -1) {
	  i++;
	  debug_printf("cups-browsed: Reading command line: %s\n", argv[i]);
	  val = argv[i];
	} else {
	  fprintf(stderr, "cups-browsed: Expected auto shutdown setting after \"--autoshutdown\" option.\n");
	  exit(1);
	}
	if (!strcasecmp(val, "On") || !strcasecmp(val, "Yes") ||
	    !strcasecmp(val, "True") || !strcasecmp(val, "1")) {
	  autoshutdown = 1;
	  debug_printf("cups-browsed: Turning on auto shutdown mode.\n");
	} else if (!strcasecmp(val, "Off") || !strcasecmp(val, "No") ||
	    !strcasecmp(val, "False") || !strcasecmp(val, "0")) {
	  autoshutdown = 0;
	  debug_printf("cups-browsed: Turning off auto shutdown mode (permanent mode).\n");
	} else if (!strcasecmp(val, "avahi")) {
	  autoshutdown_avahi = 1;
	  debug_printf("cups-browsed: Turning on auto shutdown control by appearing and disappearing of the Avahi server.\n");
	} else if (strcasecmp(val, "none")) {
	  debug_printf("cups-browsed: Unknown mode '%s'\n", val);
	  exit(1);
	}
      }
  }

  /* Set the CUPS_SERVER environment variable to assure that cups-browsed
     always works with the local CUPS daemon and never with a remote one
     specified by a client.conf file */
#ifdef CUPS_DEFAULT_DOMAINSOCKET
  if (DomainSocket == NULL)
    DomainSocket = CUPS_DEFAULT_DOMAINSOCKET;
#endif
  if (DomainSocket != NULL) {
    struct stat sockinfo;               /* Domain socket information */
    if (!stat(DomainSocket, &sockinfo) &&
        (sockinfo.st_mode & S_IRWXO) == S_IRWXO)
      setenv("CUPS_SERVER", DomainSocket, 1);
    else
      setenv("CUPS_SERVER", "localhost", 1);
  } else
    setenv("CUPS_SERVER", "localhost", 1);

  if (BrowseLocalProtocols & BROWSE_DNSSD) {
    fprintf(stderr, "Local support for DNSSD not implemented\n");
    BrowseLocalProtocols &= ~BROWSE_DNSSD;
  }

#ifndef HAVE_AVAHI
  if (BrowseRemoteProtocols & BROWSE_DNSSD) {
    fprintf(stderr, "Remote support for DNSSD not supported\n");
    BrowseRemoteProtocols &= ~BROWSE_DNSSD;
  }
#endif /* HAVE_AVAHI */

  /* Wait for CUPS daemon to start */
  while ((http = http_connect_local ()) == NULL)
    sleep(1);

  /* Initialise the array of network interfaces */
  netifs = cupsArrayNew(compare_pointers, NULL);
  update_netifs (NULL);

  local_printers = g_hash_table_new_full (g_str_hash,
					  g_str_equal,
					  g_free,
					  free_local_printer);

  /* Read out the currently defined CUPS queues and find the ones which we
     have added in an earlier session */
  update_local_printers ();
  remote_printers = cupsArrayNew((cups_array_func_t)compare_remote_printers,
				 NULL);
  g_hash_table_foreach (local_printers, find_previous_queue, NULL);

  /* Redirect SIGINT and SIGTERM so that we do a proper shutdown, removing
     the CUPS queues which we have created
     Use SIGUSR1 and SIGUSR2 to turn off and turn on auto shutdown mode
     resp. */
#ifdef HAVE_SIGSET /* Use System V signals over POSIX to avoid bugs */
  sigset(SIGTERM, sigterm_handler);
  sigset(SIGINT, sigterm_handler);
  sigset(SIGUSR1, sigusr1_handler);
  sigset(SIGUSR2, sigusr2_handler);
  debug_printf("cups-browsed: Using signal handler SIGSET\n");
#elif defined(HAVE_SIGACTION)
  struct sigaction action; /* Actions for POSIX signals */
  memset(&action, 0, sizeof(action));
  sigemptyset(&action.sa_mask);
  sigaddset(&action.sa_mask, SIGTERM);
  action.sa_handler = sigterm_handler;
  sigaction(SIGTERM, &action, NULL);
  sigemptyset(&action.sa_mask);
  sigaddset(&action.sa_mask, SIGINT);
  action.sa_handler = sigterm_handler;
  sigaction(SIGINT, &action, NULL);
  sigemptyset(&action.sa_mask);
  sigaddset(&action.sa_mask, SIGUSR1);
  action.sa_handler = sigusr1_handler;
  sigaction(SIGUSR1, &action, NULL);
  sigemptyset(&action.sa_mask);
  sigaddset(&action.sa_mask, SIGUSR2);
  action.sa_handler = sigusr2_handler;
  sigaction(SIGUSR2, &action, NULL);
  debug_printf("cups-browsed: Using signal handler SIGACTION\n");
#else
  signal(SIGTERM, sigterm_handler);
  signal(SIGINT, sigterm_handler);
  signal(SIGUSR1, sigusr1_handler);
  signal(SIGUSR2, sigusr2_handler);
  debug_printf("cups-browsed: Using signal handler SIGNAL\n");
#endif /* HAVE_SIGSET */

#ifdef HAVE_AVAHI
  if (autoshutdown_avahi)
    autoshutdown = 1;
  avahi_init();
#endif /* HAVE_AVAHI */

  if (BrowseLocalProtocols & BROWSE_CUPS ||
      BrowseRemoteProtocols & BROWSE_CUPS) {
    /* Set up our CUPS Browsing socket */
    browsesocket = socket (AF_INET, SOCK_DGRAM, 0);
    if (browsesocket == -1) {
      debug_printf("cups-browsed: failed to create CUPS Browsing socket: %s\n",
		   strerror (errno));
    } else {
      struct sockaddr_in addr;
      memset (&addr, 0, sizeof (addr));
      addr.sin_addr.s_addr = htonl (INADDR_ANY);
      addr.sin_family = AF_INET;
      addr.sin_port = htons (BrowsePort);
      if (bind (browsesocket, (struct sockaddr *)&addr, sizeof (addr))) {
	debug_printf("cups-browsed: failed to bind CUPS Browsing socket: %s\n",
		     strerror (errno));
	close (browsesocket);
	browsesocket = -1;
      } else {
	int on = 1;
	if (setsockopt (browsesocket, SOL_SOCKET, SO_BROADCAST,
			&on, sizeof (on))) {
	  debug_printf("cups-browsed: failed to allow broadcast: %s\n",
		       strerror (errno));
	  BrowseLocalProtocols &= ~BROWSE_CUPS;
	}
      }
    }

    if (browsesocket == -1) {
      BrowseLocalProtocols &= ~BROWSE_CUPS;
      BrowseRemoteProtocols &= ~BROWSE_CUPS;
    }
  }

  if (BrowseLocalProtocols == 0 &&
      BrowseRemoteProtocols == 0 &&
      !BrowsePoll) {
    debug_printf("cups-browsed: nothing left to do\n");
    ret = 0;
    goto fail;
  }

  /* Override the default password callback so we don't end up
   * prompting for it. */
  cupsSetPasswordCB2 (password_callback, NULL);

  /* Watch NetworkManager for network interface changes */
  proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
					 G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
					 NULL, /* GDBusInterfaceInfo */
					 "org.freedesktop.NetworkManager",
					 "/org/freedesktop/NetworkManager",
					 "org.freedesktop.NetworkManager",
					 NULL, /* GCancellable */
					 NULL); /* GError */

  if (proxy)
    g_signal_connect (proxy,
		      "g-properties-changed",
		      G_CALLBACK (nm_properties_changed),
		      NULL);

  /* Run the main loop */
  gmainloop = g_main_loop_new (NULL, FALSE);
  recheck_timer ();

  if (BrowseRemoteProtocols & BROWSE_CUPS) {
    GIOChannel *browse_channel = g_io_channel_unix_new (browsesocket);
    g_io_channel_set_close_on_unref (browse_channel, FALSE);
    g_io_add_watch (browse_channel, G_IO_IN, process_browse_data, NULL);
  }

  if (BrowseLocalProtocols & BROWSE_CUPS) {
      debug_printf ("cups-browsed: will send browse data every %ds\n",
		    BrowseInterval);
      g_idle_add (send_browse_data, NULL);
  }

  if (BrowsePoll) {
    size_t index;
    for (index = 0;
	 index < NumBrowsePoll;
	 index++) {
      debug_printf ("cups-browsed: will browse poll %s every %ds\n",
		    BrowsePoll[index]->server, BrowseInterval);
      g_idle_add (browse_poll, BrowsePoll[index]);
    }
  }

  /* If auto shutdown is active and we do not find any printers initially,
     schedule the shutdown in autoshutdown_timeout seconds */
  if (autoshutdown && !autoshutdown_exec_id &&
      cupsArrayCount(remote_printers) == 0) {
    debug_printf ("cups-browsed: No printers found to make available, shutting down in %d sec...\n", autoshutdown_timeout);
    autoshutdown_exec_id =
      g_timeout_add_seconds (autoshutdown_timeout, autoshutdown_execute, NULL);
  }

  g_main_loop_run (gmainloop);

  debug_printf("cups-browsed: main loop exited\n");
  g_main_loop_unref (gmainloop);
  gmainloop = NULL;
  ret = 0;

fail:

  /* Clean up things */

  if (proxy)
    g_object_unref (proxy);

  /* Remove all queues which we have set up */
  for (p = (remote_printer_t *)cupsArrayFirst(remote_printers);
       p; p = (remote_printer_t *)cupsArrayNext(remote_printers)) {
    p->status = STATUS_DISAPPEARED;
    p->timeout = time(NULL) + TIMEOUT_IMMEDIATELY;
  }
  handle_cups_queues(NULL);

  if (BrowsePoll) {
    size_t index;
    for (index = 0;
	 index < NumBrowsePoll;
	 index++) {
      if (BrowsePoll[index]->can_subscribe &&
	  BrowsePoll[index]->subscription_id != -1)
	browse_poll_cancel_subscription (BrowsePoll[index]);

      free (BrowsePoll[index]->server);
      g_list_free_full (BrowsePoll[index]->printers,
			browsepoll_printer_free);
      free (BrowsePoll[index]);
    }

    free (BrowsePoll);
  }

  if (local_printers_context) {
    browse_poll_cancel_subscription (local_printers_context);
    g_list_free_full (local_printers_context->printers,
		      browsepoll_printer_free);
    free (local_printers_context);
  }

  http_close_local ();

#ifdef HAVE_AVAHI
  avahi_shutdown();
#endif /* HAVE_AVAHI */

  if (browsesocket != -1)
      close (browsesocket);

  g_hash_table_destroy (local_printers);

  if (BrowseLocalProtocols & BROWSE_CUPS)
    g_list_free_full (browse_data, browse_data_free);

  return ret;
}


/*
 * The code below is borrowed from the CUPS 2.1.x upstream repository
 * (via patches attached to https://www.cups.org/str.php?L4258). This
 * allows for automatic PPD generation already with CUPS versions older
 * than CUPS 2.1.x. We have also an additional test and development
 * platform for this code. Taken from cups/ppd-cache.c,
 * cups/string-private.h, cups/string.c.
 * 
 * The advantage of PPD generation instead of working with System V
 * interface scripts is that the print dialogs of the clients do not
 * need to ask the printer for its options via IPP. So we have access
 * to the options with the current PPD-based dialogs and can even share
 * the automatically created print queue to other CUPS-based machines
 * without problems.
 */


typedef struct _pwg_finishings_s	/**** PWG finishings mapping data ****/
{
  ipp_finishings_t	value;		/* finishings value */
  int			num_options;	/* Number of options to apply */
  cups_option_t		*options;	/* Options to apply */
} _pwg_finishings_t;

#define _PWG_EQUIVALENT(x, y)	(abs((x)-(y)) < 2)

static void	pwg_ppdize_name(const char *ipp, char *name, size_t namesize);
static void	pwg_ppdize_resolution(ipp_attribute_t *attr, int element, int *xres, int *yres, char *name, size_t namesize);

int			/* O - 1 on match, 0 otherwise */
_cups_isalnum(int ch)			/* I - Character to test */
{
  return ((ch >= '0' && ch <= '9') ||
          (ch >= 'A' && ch <= 'Z') ||
          (ch >= 'a' && ch <= 'z'));
}

int			/* O - 1 on match, 0 otherwise */
_cups_isalpha(int ch)			/* I - Character to test */
{
  return ((ch >= 'A' && ch <= 'Z') ||
          (ch >= 'a' && ch <= 'z'));
}

int			/* O - 1 on match, 0 otherwise */
_cups_islower(int ch)			/* I - Character to test */
{
  return (ch >= 'a' && ch <= 'z');
}

int			/* O - 1 on match, 0 otherwise */
_cups_isspace(int ch)			/* I - Character to test */
{
  return (ch == ' ' || ch == '\f' || ch == '\n' || ch == '\r' || ch == '\t' ||
          ch == '\v');
}

int			/* O - 1 on match, 0 otherwise */
_cups_isupper(int ch)			/* I - Character to test */
{
  return (ch >= 'A' && ch <= 'Z');
}

int			/* O - Converted character */
_cups_tolower(int ch)			/* I - Character to convert */
{
  return (_cups_isupper(ch) ? ch - 'A' + 'a' : ch);
}

int			/* O - Converted character */
_cups_toupper(int ch)			/* I - Character to convert */
{
  return (_cups_islower(ch) ? ch - 'a' + 'A' : ch);
}

/*
 * '_cups_strcasecmp()' - Do a case-insensitive comparison.
 */

int				/* O - Result of comparison (-1, 0, or 1) */
_cups_strcasecmp(const char *s,	/* I - First string */
                 const char *t)	/* I - Second string */
{
  while (*s != '\0' && *t != '\0')
  {
    if (_cups_tolower(*s) < _cups_tolower(*t))
      return (-1);
    else if (_cups_tolower(*s) > _cups_tolower(*t))
      return (1);

    s ++;
    t ++;
  }

  if (*s == '\0' && *t == '\0')
    return (0);
  else if (*s != '\0')
    return (1);
  else
    return (-1);
}

/*
 * '_cups_strncasecmp()' - Do a case-insensitive comparison on up to N chars.
 */

int					/* O - Result of comparison (-1, 0, or 1) */
_cups_strncasecmp(const char *s,	/* I - First string */
                  const char *t,	/* I - Second string */
		  size_t     n)		/* I - Maximum number of characters to compare */
{
  while (*s != '\0' && *t != '\0' && n > 0)
  {
    if (_cups_tolower(*s) < _cups_tolower(*t))
      return (-1);
    else if (_cups_tolower(*s) > _cups_tolower(*t))
      return (1);

    s ++;
    t ++;
    n --;
  }

  if (n == 0)
    return (0);
  else if (*s == '\0' && *t == '\0')
    return (0);
  else if (*s != '\0')
    return (1);
  else
    return (-1);
}

#ifndef HAVE_STRLCPY
/*
 * '_cups_strlcpy()' - Safely copy two strings.
 */

size_t					/* O - Length of string */
strlcpy(char       *dst,		/* O - Destination string */
	const char *src,		/* I - Source string */
	size_t      size)		/* I - Size of destination string buffer */
{
  size_t	srclen;			/* Length of source string */


 /*
  * Figure out how much room is needed...
  */

  size --;

  srclen = strlen(src);

 /*
  * Copy the appropriate amount...
  */

  if (srclen > size)
    srclen = size;

  memmove(dst, src, srclen);
  dst[srclen] = '\0';

  return (srclen);
}
#endif /* !HAVE_STRLCPY */

/*
 * '_ppdCreateFromIPP()' - Create a PPD file describing the capabilities
 *                         of an IPP printer.
 */

char *					/* O - PPD filename or NULL on error */
_ppdCreateFromIPP(char   *buffer,	/* I - Filename buffer */
                  size_t bufsize,	/* I - Size of filename buffer */
		  ipp_t  *response)	/* I - Get-Printer-Attributes response */
{
  cups_file_t		*fp;		/* PPD file */
  ipp_attribute_t	*attr,		/* xxx-supported */
			*defattr,	/* xxx-default */
			*x_dim, *y_dim;	/* Media dimensions */
  ipp_t			*media_size;	/* Media size collection */
  char			make[256],	/* Make and model */
			*model,		/* Model name */
			ppdname[PPD_MAX_NAME];
		    			/* PPD keyword */
  int			i, j,		/* Looping vars */
			count,		/* Number of values */
			bottom,		/* Largest bottom margin */
			left,		/* Largest left margin */
			right,		/* Largest right margin */
			top;		/* Largest top margin */
  pwg_media_t		*pwg;		/* PWG media size */
  int			xres, yres;	/* Resolution values */


 /*
  * Range check input...
  */

  if (buffer)
    *buffer = '\0';

  if (!buffer || bufsize < 1 || !response)
    return (NULL);

 /*
  * Open a temporary file for the PPD...
  */

  if ((fp = cupsTempFile2(buffer, (int)bufsize)) == NULL)
    return (NULL);

 /*
  * Standard stuff for PPD file...
  */

  cupsFilePuts(fp, "*PPD-Adobe: \"4.3\"\n");
  cupsFilePuts(fp, "*FormatVersion: \"4.3\"\n");
  cupsFilePrintf(fp, "*FileVersion: \"%d.%d\"\n", CUPS_VERSION_MAJOR, CUPS_VERSION_MINOR);
  cupsFilePuts(fp, "*LanguageVersion: English\n");
  cupsFilePuts(fp, "*LanguageEncoding: ISOLatin1\n");
  cupsFilePuts(fp, "*PSVersion: \"(3010.000) 0\"\n");
  cupsFilePuts(fp, "*LanguageLevel: \"3\"\n");
  cupsFilePuts(fp, "*FileSystem: False\n");
  cupsFilePuts(fp, "*PCFileName: \"ippeve.ppd\"\n");

  if ((attr = ippFindAttribute(response, "printer-make-and-model", IPP_TAG_TEXT)) != NULL)
    strlcpy(make, ippGetString(attr, 0, NULL), sizeof(make));
  else
    strlcpy(make, "Unknown Printer", sizeof(make));

  if (!_cups_strncasecmp(make, "Hewlett Packard ", 16) ||
      !_cups_strncasecmp(make, "Hewlett-Packard ", 16))
  {
    model = make + 16;
    strlcpy(make, "HP", sizeof(make));
  }
  else if ((model = strchr(make, ' ')) != NULL)
    *model++ = '\0';
  else
    model = make;

  cupsFilePrintf(fp, "*Manufacturer: \"%s\"\n", make);
  cupsFilePrintf(fp, "*ModelName: \"%s\"\n", model);
  cupsFilePrintf(fp, "*Product: \"(%s)\"\n", model);
  cupsFilePrintf(fp, "*NickName: \"%s\"\n", model);
  cupsFilePrintf(fp, "*ShortNickName: \"%s\"\n", model);

  if ((attr = ippFindAttribute(response, "color-supported", IPP_TAG_BOOLEAN)) != NULL && ippGetBoolean(attr, 0))
    cupsFilePuts(fp, "*ColorDevice: True\n");
  else
    cupsFilePuts(fp, "*ColorDevice: False\n");

  cupsFilePrintf(fp, "*cupsVersion: %d.%d\n", CUPS_VERSION_MAJOR, CUPS_VERSION_MINOR);
  cupsFilePuts(fp, "*cupsSNMPSupplies: False\n");
  cupsFilePuts(fp, "*cupsLanguages: \"en\"\n");

 /*
  * Filters...
  */

  if ((attr = ippFindAttribute(response, "document-format-supported", IPP_TAG_MIMETYPE)) != NULL)
  {
    int formatfound = 0;

    for (i = 0, count = ippGetCount(attr); i < count; i ++)
    {
      const char *format = ippGetString(attr, i, NULL);
					/* PDL */
#if 0
      if (!_cups_strcasecmp(format, "application/pdf"))
        cupsFilePuts(fp, "*cupsFilter2: \"application/vnd.cups-pdf application/pdf 10 -\"\n");
      else if (!_cups_strcasecmp(format, "application/postscript"))
        cupsFilePuts(fp, "*cupsFilter2: \"application/vnd.cups-postscript application/postscript 10 -\"\n");
      else if (_cups_strcasecmp(format, "application/octet-stream") && _cups_strcasecmp(format, "application/vnd.hp-pcl") && _cups_strcasecmp(format, "text/plain"))
        cupsFilePrintf(fp, "*cupsFilter2: \"%s %s 10 -\"\n", format, format);
#endif
      /* For now cups-filters only supports these PPD files with PWG Raster
	 and PostScript, so create a PPD only for these formats */
      if (!_cups_strcasecmp(format, "image/pwg-raster")) {
        cupsFilePuts(fp, "*cupsFilter2: \"image/pwg-raster image/pwg-raster 10 -\"\n");
	formatfound = 1;
      } else if (!_cups_strcasecmp(format, "application/postscript")) {
        cupsFilePuts(fp, "*cupsFilter2: \"application/vnd.cups-postscript application/postscript 10 -\"\n");
	formatfound = 1;
      }
    }
    if (formatfound == 0) {
      debug_printf("cups-browsed: No data format suitable for PPD auto-generation supported by the printer, not generating PPD\n");
      unlink(buffer);
      return(NULL);
    }
  }

 /*
  * PageSize/PageRegion/ImageableArea/PaperDimension
  */

  if ((attr = ippFindAttribute(response, "media-bottom-margin-supported", IPP_TAG_INTEGER)) != NULL)
  {
    for (i = 1, bottom = ippGetInteger(attr, 0), count = ippGetCount(attr); i < count; i ++)
      if (ippGetInteger(attr, i) > bottom)
        bottom = ippGetInteger(attr, i);
  }
  else
    bottom = 1270;

  if ((attr = ippFindAttribute(response, "media-left-margin-supported", IPP_TAG_INTEGER)) != NULL)
  {
    for (i = 1, left = ippGetInteger(attr, 0), count = ippGetCount(attr); i < count; i ++)
      if (ippGetInteger(attr, i) > left)
        left = ippGetInteger(attr, i);
  }
  else
    left = 635;

  if ((attr = ippFindAttribute(response, "media-right-margin-supported", IPP_TAG_INTEGER)) != NULL)
  {
    for (i = 1, right = ippGetInteger(attr, 0), count = ippGetCount(attr); i < count; i ++)
      if (ippGetInteger(attr, i) > right)
        right = ippGetInteger(attr, i);
  }
  else
    right = 635;

  if ((attr = ippFindAttribute(response, "media-top-margin-supported", IPP_TAG_INTEGER)) != NULL)
  {
    for (i = 1, top = ippGetInteger(attr, 0), count = ippGetCount(attr); i < count; i ++)
      if (ippGetInteger(attr, i) > top)
        top = ippGetInteger(attr, i);
  }
  else
    top = 1270;

  if ((defattr = ippFindAttribute(response, "media-col-default", IPP_TAG_BEGIN_COLLECTION)) != NULL)
  {
    if ((attr = ippFindAttribute(ippGetCollection(defattr, 0), "media-size", IPP_TAG_BEGIN_COLLECTION)) != NULL)
    {
      media_size = ippGetCollection(attr, 0);
      x_dim      = ippFindAttribute(media_size, "x-dimension", IPP_TAG_INTEGER);
      y_dim      = ippFindAttribute(media_size, "y-dimension", IPP_TAG_INTEGER);

      if (x_dim && y_dim)
      {
        pwg = pwgMediaForSize(ippGetInteger(x_dim, 0), ippGetInteger(y_dim, 0));
	strlcpy(ppdname, pwg->ppd, sizeof(ppdname));
      }
      else
	strlcpy(ppdname, "Unknown", sizeof(ppdname));
    }
    else
      strlcpy(ppdname, "Unknown", sizeof(ppdname));
  }

  if ((attr = ippFindAttribute(response, "media-size-supported", IPP_TAG_BEGIN_COLLECTION)) != NULL)
  {
    cupsFilePrintf(fp, "*OpenUI *PageSize: PickOne\n"
		       "*OrderDependency: 10 AnySetup *PageSize\n"
                       "*DefaultPageSize: %s\n", ppdname);
    if (ippGetCount(attr) == 0) {
      debug_printf("cups-browsed: No page sizes reported as supported by the printer, not generating PPD\n");
      unlink(buffer);
      return(NULL);
    }
    for (i = 0, count = ippGetCount(attr); i < count; i ++)
    {
      media_size = ippGetCollection(attr, i);
      x_dim      = ippFindAttribute(media_size, "x-dimension", IPP_TAG_INTEGER);
      y_dim      = ippFindAttribute(media_size, "y-dimension", IPP_TAG_INTEGER);

      if (x_dim && y_dim)
      {
        pwg = pwgMediaForSize(ippGetInteger(x_dim, 0), ippGetInteger(y_dim, 0));

        cupsFilePrintf(fp, "*PageSize %s: \"<</PageSize[%.1f %.1f]>>setpagedevice\"\n", pwg->ppd, pwg->width * 72.0 / 2540.0, pwg->length * 72.0 / 2540.0);
      }
    }
    cupsFilePuts(fp, "*CloseUI: *PageSize\n");

    cupsFilePrintf(fp, "*OpenUI *PageRegion: PickOne\n"
                       "*OrderDependency: 10 AnySetup *PageRegion\n"
                       "*DefaultPageRegion: %s\n", ppdname);
    for (i = 0, count = ippGetCount(attr); i < count; i ++)
    {
      media_size = ippGetCollection(attr, i);
      x_dim      = ippFindAttribute(media_size, "x-dimension", IPP_TAG_INTEGER);
      y_dim      = ippFindAttribute(media_size, "y-dimension", IPP_TAG_INTEGER);

      if (x_dim && y_dim)
      {
        pwg = pwgMediaForSize(ippGetInteger(x_dim, 0), ippGetInteger(y_dim, 0));

        cupsFilePrintf(fp, "*PageRegion %s: \"<</PageSize[%.1f %.1f]>>setpagedevice\"\n", pwg->ppd, pwg->width * 72.0 / 2540.0, pwg->length * 72.0 / 2540.0);
      }
    }
    cupsFilePuts(fp, "*CloseUI: *PageRegion\n");

    cupsFilePrintf(fp, "*DefaultImageableArea: %s\n"
		       "*DefaultPaperDimension: %s\n", ppdname, ppdname);
    for (i = 0, count = ippGetCount(attr); i < count; i ++)
    {
      media_size = ippGetCollection(attr, i);
      x_dim      = ippFindAttribute(media_size, "x-dimension", IPP_TAG_INTEGER);
      y_dim      = ippFindAttribute(media_size, "y-dimension", IPP_TAG_INTEGER);

      if (x_dim && y_dim)
      {
        pwg = pwgMediaForSize(ippGetInteger(x_dim, 0), ippGetInteger(y_dim, 0));

        cupsFilePrintf(fp, "*ImageableArea %s: \"%.1f %.1f %.1f %.1f\"\n", pwg->ppd, left * 72.0 / 2540.0, bottom * 72.0 / 2540.0, (pwg->width - right) * 72.0 / 2540.0, (pwg->length - top) * 72.0 / 2540.0);
        cupsFilePrintf(fp, "*PaperDimension %s: \"%.1f %.1f\"\n", pwg->ppd, pwg->width * 72.0 / 2540.0, pwg->length * 72.0 / 2540.0);
      }
    }
  } else {
    debug_printf("cups-browsed: No page sizes reported as supported by the printer, not generating PPD\n");
    unlink(buffer);
    return(NULL);
  }

 /*
  * InputSlot...
  */

  if ((attr = ippFindAttribute(ippGetCollection(defattr, 0), "media-source", IPP_TAG_KEYWORD)) != NULL)
    pwg_ppdize_name(ippGetString(attr, 0, NULL), ppdname, sizeof(ppdname));
  else
    strlcpy(ppdname, "Unknown", sizeof(ppdname));

  if ((attr = ippFindAttribute(response, "media-source-supported", IPP_TAG_KEYWORD)) != NULL && (count = ippGetCount(attr)) > 1)
  {
    static const char * const sources[][2] =
    {
      { "Auto", "Automatic" },
      { "Main", "Main" },
      { "Alternate", "Alternate" },
      { "LargeCapacity", "Large Capacity" },
      { "Manual", "Manual" },
      { "Envelope", "Envelope" },
      { "Disc", "Disc" },
      { "Photo", "Photo" },
      { "Hagaki", "Hagaki" },
      { "MainRoll", "Main Roll" },
      { "AlternateRoll", "Alternate Roll" },
      { "Top", "Top" },
      { "Middle", "Middle" },
      { "Bottom", "Bottom" },
      { "Side", "Side" },
      { "Left", "Left" },
      { "Right", "Right" },
      { "Center", "Center" },
      { "Rear", "Rear" },
      { "ByPassTray", "Multipurpose" },
      { "Tray1", "Tray 1" },
      { "Tray2", "Tray 2" },
      { "Tray3", "Tray 3" },
      { "Tray4", "Tray 4" },
      { "Tray5", "Tray 5" },
      { "Tray6", "Tray 6" },
      { "Tray7", "Tray 7" },
      { "Tray8", "Tray 8" },
      { "Tray9", "Tray 9" },
      { "Tray10", "Tray 10" },
      { "Tray11", "Tray 11" },
      { "Tray12", "Tray 12" },
      { "Tray13", "Tray 13" },
      { "Tray14", "Tray 14" },
      { "Tray15", "Tray 15" },
      { "Tray16", "Tray 16" },
      { "Tray17", "Tray 17" },
      { "Tray18", "Tray 18" },
      { "Tray19", "Tray 19" },
      { "Tray20", "Tray 20" },
      { "Roll1", "Roll 1" },
      { "Roll2", "Roll 2" },
      { "Roll3", "Roll 3" },
      { "Roll4", "Roll 4" },
      { "Roll5", "Roll 5" },
      { "Roll6", "Roll 6" },
      { "Roll7", "Roll 7" },
      { "Roll8", "Roll 8" },
      { "Roll9", "Roll 9" },
      { "Roll10", "Roll 10" }
    };

    cupsFilePrintf(fp, "*OpenUI *InputSlot: PickOne\n"
                       "*OrderDependency: 10 AnySetup *InputSlot\n"
                       "*DefaultInputSlot: %s\n", ppdname);
    for (i = 0, count = ippGetCount(attr); i < count; i ++)
    {
      pwg_ppdize_name(ippGetString(attr, i, NULL), ppdname, sizeof(ppdname));

      for (j = 0; j < (int)(sizeof(sources) / sizeof(sources[0])); j ++)
        if (!strcmp(sources[j][0], ppdname))
	{
	  cupsFilePrintf(fp, "*InputSlot %s/%s: \"<</MediaPosition %d>>setpagedevice\"\n", ppdname, sources[j][1], j);
	  break;
	}
    }
    cupsFilePuts(fp, "*CloseUI: *InputSlot\n");
  }

 /*
  * MediaType...
  */

  if ((attr = ippFindAttribute(ippGetCollection(defattr, 0), "media-type", IPP_TAG_KEYWORD)) != NULL)
    pwg_ppdize_name(ippGetString(attr, 0, NULL), ppdname, sizeof(ppdname));
  else
    strlcpy(ppdname, "Unknown", sizeof(ppdname));

  if ((attr = ippFindAttribute(response, "media-type-supported", IPP_TAG_KEYWORD)) != NULL && (count = ippGetCount(attr)) > 1)
  {
    static const char * const types[][2] =
    {					/* Media type strings (far from complete) */
      { "Auto", "Automatic" },
      { "Cardstock", "Cardstock" },
      { "Disc", "CD/DVD/Bluray" },
      { "Envelope", "Envelope" },
      { "Labels", "Label" },
      { "Other", "Other" },
      { "Photographic", "Photo" },
      { "PhotographicGlossy", "Glossy Photo" },
      { "PhotographicHighGloss", "High-Gloss Photo" },
      { "PhotographicMatte", "Matte Photo" },
      { "PhotographicSatin", "Satin Photo" },
      { "PhotographicSemiGloss", "Semi-Gloss Photo" },
      { "Stationery", "Plain Paper" },
      { "StationeryLetterhead", "Letterhead" },
      { "Transparency", "Transparency" }
    };

    cupsFilePrintf(fp, "*OpenUI *MediaType: PickOne\n"
                       "*OrderDependency: 10 AnySetup *MediaType\n"
                       "*DefaultMediaType: %s\n", ppdname);
    for (i = 0, count = ippGetCount(attr); i < count; i ++)
    {
      pwg_ppdize_name(ippGetString(attr, i, NULL), ppdname, sizeof(ppdname));

      for (j = 0; j < (int)(sizeof(types) / sizeof(types[0])); j ++)
        if (!strcmp(types[j][0], ppdname))
	{
	  cupsFilePrintf(fp, "*MediaType %s/%s: \"<</MediaType(%s)>>setpagedevice\"\n", ppdname, types[j][1], ppdname);
	  break;
	}

      if (j >= (int)(sizeof(types) / sizeof(types[0])))
	cupsFilePrintf(fp, "*MediaType %s: \"<</MediaType(%s)>>setpagedevice\"\n", ppdname, ppdname);

    }
    cupsFilePuts(fp, "*CloseUI: *MediaType\n");
  }

 /*
  * ColorModel...
  */

  if ((attr = ippFindAttribute(response, "pwg-raster-document-type-supported", IPP_TAG_KEYWORD)) == NULL)
    attr = ippFindAttribute(response, "print-color-mode-supported", IPP_TAG_KEYWORD);

  if (attr && ippGetCount(attr) > 0)
  {
    const char *default_color = NULL;	/* Default */

    cupsFilePuts(fp, "*OpenUI *ColorModel/Color Mode: PickOne\n"
		     "*OrderDependency: 10 AnySetup *ColorModel\n");
    for (i = 0, count = ippGetCount(attr); i < count; i ++)
    {
      const char *keyword = ippGetString(attr, i, NULL);
					/* Keyword for color/bit depth */

      if (!strcmp(keyword, "black_1") || !strcmp(keyword, "bi-level") || !strcmp(keyword, "process-bi-level"))
      {
        cupsFilePuts(fp, "*ColorModel FastGray/Fast Grayscale: \"<</cupsColorSpace 3/cupsBitsPerColor 1/cupsColorOrder 0/cupsCompression 0>>setpagedevice\"\n");

        if (!default_color)
	  default_color = "FastGray";
      }
      else if (!strcmp(keyword, "sgray_8") || !strcmp(keyword, "monochrome") || !strcmp(keyword, "process-monochrome"))
      {
        cupsFilePuts(fp, "*ColorModel Gray/Grayscale: \"<</cupsColorSpace 18/cupsBitsPerColor 8/cupsColorOrder 0/cupsCompression 0>>setpagedevice\"\n");

        if (!default_color || !strcmp(default_color, "FastGray"))
	  default_color = "Gray";
      }
      else if (!strcmp(keyword, "srgb_8") || !strcmp(keyword, "color"))
      {
        cupsFilePuts(fp, "*ColorModel RGB/Color: \"<</cupsColorSpace 19/cupsBitsPerColor 8/cupsColorOrder 0/cupsCompression 0>>setpagedevice\"\n");

	default_color = "RGB";
      }
    }

    if (default_color)
      cupsFilePrintf(fp, "*DefaultColorModel: %s\n", default_color);

    cupsFilePuts(fp, "*CloseUI: *ColorModel\n");
  }

 /*
  * Duplex...
  */

  if ((attr = ippFindAttribute(response, "sides-supported", IPP_TAG_KEYWORD)) != NULL && ippContainsString(attr, "two-sided-long-edge"))
  {
    cupsFilePuts(fp, "*OpenUI *Duplex/2-Sided Printing: PickOne\n"
                     "*OrderDependency: 10 AnySetup *Duplex\n"
                     "*DefaultDuplex: None\n"
                     "*Duplex None/Off (1-Sided): \"<</Duplex false>>setpagedevice\"\n"
                     "*Duplex DuplexNoTumble/Long-Edge (Portrait): \"<</Duplex true/Tumble false>>setpagedevice\"\n"
                     "*Duplex DuplexTumble/Short-Edge (Landscape): \"<</Duplex true/Tumble true>>setpagedevice\"\n"
                     "*CloseUI: *Duplex\n");

    if ((attr = ippFindAttribute(response, "pwg-raster-document-sheet-back", IPP_TAG_KEYWORD)) != NULL)
    {
      const char *keyword = ippGetString(attr, 0, NULL);
					/* Keyword value */

      if (!strcmp(keyword, "flipped"))
        cupsFilePuts(fp, "*cupsBackSide: Flipped\n");
      else if (!strcmp(keyword, "manual-tumble"))
        cupsFilePuts(fp, "*cupsBackSide: ManualTumble\n");
      else if (!strcmp(keyword, "normal"))
        cupsFilePuts(fp, "*cupsBackSide: Normal\n");
      else
        cupsFilePuts(fp, "*cupsBackSide: Rotated\n");
    }
    else if ((attr = ippFindAttribute(response, "urf-supported", IPP_TAG_KEYWORD)) != NULL)
    {
      for (i = 0, count = ippGetCount(attr); i < count; i ++)
      {
	const char *dm = ippGetString(attr, i, NULL);
					  /* DM value */

	if (!_cups_strcasecmp(dm, "DM1"))
	{
	  cupsFilePuts(fp, "*cupsBackSide: Normal\n");
	  break;
	}
	else if (!_cups_strcasecmp(dm, "DM2"))
	{
	  cupsFilePuts(fp, "*cupsBackSide: Flipped\n");
	  break;
	}
	else if (!_cups_strcasecmp(dm, "DM3"))
	{
	  cupsFilePuts(fp, "*cupsBackSide: Rotated\n");
	  break;
	}
	else if (!_cups_strcasecmp(dm, "DM4"))
	{
	  cupsFilePuts(fp, "*cupsBackSide: ManualTumble\n");
	  break;
	}
      }
    }
  }

 /*
  * cupsPrintQuality and DefaultResolution...
  */

  if ((attr = ippFindAttribute(response, "pwg-raster-document-resolution-supported", IPP_TAG_RESOLUTION)) != NULL)
  {
    count = ippGetCount(attr);

    pwg_ppdize_resolution(attr, count / 2, &xres, &yres, ppdname, sizeof(ppdname));
    cupsFilePrintf(fp, "*DefaultResolution: %s\n", ppdname);

    cupsFilePuts(fp, "*OpenUI *cupsPrintQuality/Print Quality: PickOne\n"
                     "*OrderDependency: 10 AnySetup *cupsPrintQuality\n"
                     "*DefaultcupsPrintQuality: Normal\n");
    if (count > 2)
    {
      pwg_ppdize_resolution(attr, 0, &xres, &yres, NULL, 0);
      cupsFilePrintf(fp, "*cupsPrintQuality Draft: \"<</HWResolution[%d %d]>>setpagedevice\"\n", xres, yres);
    }
    pwg_ppdize_resolution(attr, count / 2, &xres, &yres, NULL, 0);
    cupsFilePrintf(fp, "*cupsPrintQuality Normal: \"<</HWResolution[%d %d]>>setpagedevice\"\n", xres, yres);
    if (count > 1)
    {
      pwg_ppdize_resolution(attr, count - 1, &xres, &yres, NULL, 0);
      cupsFilePrintf(fp, "*cupsPrintQuality High: \"<</HWResolution[%d %d]>>setpagedevice\"\n", xres, yres);
    }

    cupsFilePuts(fp, "*CloseUI: *cupsPrintQuality\n");
  }
  else if ((attr = ippFindAttribute(response, "urf-supported", IPP_TAG_KEYWORD)) != NULL)
  {
    int lowdpi = 0, hidpi = 0;		/* Lower and higher resolution */

    for (i = 0, count = ippGetCount(attr); i < count; i ++)
    {
      const char *rs = ippGetString(attr, i, NULL);
					/* RS value */

      if (_cups_strncasecmp(rs, "RS", 2))
        continue;

      lowdpi = atoi(rs + 2);
      if ((rs = strrchr(rs, '-')) != NULL)
        hidpi = atoi(rs + 1);
      else
        hidpi = lowdpi;
      break;
    }

    if (lowdpi == 0)
    {
     /*
      * Invalid "urf-supported" value...
      */

      cupsFilePuts(fp, "*DefaultResolution: 300dpi\n");
    }
    else
    {
     /*
      * Generate print qualities based on low and high DPIs...
      */

      cupsFilePrintf(fp, "*DefaultResolution: %ddpi\n", lowdpi);

      cupsFilePuts(fp, "*OpenUI *cupsPrintQuality/Print Quality: PickOne\n"
		       "*OrderDependency: 10 AnySetup *cupsPrintQuality\n"
		       "*DefaultcupsPrintQuality: Normal\n");
      if ((lowdpi & 1) == 0)
	cupsFilePrintf(fp, "*cupsPrintQuality Draft: \"<</HWResolution[%d %d]>>setpagedevice\"\n", lowdpi, lowdpi / 2);
      cupsFilePrintf(fp, "*cupsPrintQuality Normal: \"<</HWResolution[%d %d]>>setpagedevice\"\n", lowdpi, lowdpi);
      if (hidpi > lowdpi)
	cupsFilePrintf(fp, "*cupsPrintQuality High: \"<</HWResolution[%d %d]>>setpagedevice\"\n", hidpi, hidpi);
      cupsFilePuts(fp, "*CloseUI: *cupsPrintQuality\n");
    }
  }
  else if ((attr = ippFindAttribute(response, "printer-resolution-default", IPP_TAG_RESOLUTION)) != NULL)
  {
    pwg_ppdize_resolution(attr, 0, &xres, &yres, ppdname, sizeof(ppdname));
    cupsFilePrintf(fp, "*DefaultResolution: %s\n", ppdname);
  }
  else
    cupsFilePuts(fp, "*DefaultResolution: 300dpi\n");

 /*
  * Close up and return...
  */

  cupsFileClose(fp);

  return (buffer);
}


/*
 * '_pwgInputSlotForSource()' - Get the InputSlot name for the given PWG
 *                              media-source.
 */

const char *				/* O - InputSlot name */
_pwgInputSlotForSource(
    const char *media_source,		/* I - PWG media-source */
    char       *name,			/* I - Name buffer */
    size_t     namesize)		/* I - Size of name buffer */
{
 /*
  * Range check input...
  */

  if (!media_source || !name || namesize < PPD_MAX_NAME)
    return (NULL);

  if (_cups_strcasecmp(media_source, "main"))
    strlcpy(name, "Cassette", namesize);
  else if (_cups_strcasecmp(media_source, "alternate"))
    strlcpy(name, "Multipurpose", namesize);
  else if (_cups_strcasecmp(media_source, "large-capacity"))
    strlcpy(name, "LargeCapacity", namesize);
  else if (_cups_strcasecmp(media_source, "bottom"))
    strlcpy(name, "Lower", namesize);
  else if (_cups_strcasecmp(media_source, "middle"))
    strlcpy(name, "Middle", namesize);
  else if (_cups_strcasecmp(media_source, "top"))
    strlcpy(name, "Upper", namesize);
  else if (_cups_strcasecmp(media_source, "rear"))
    strlcpy(name, "Rear", namesize);
  else if (_cups_strcasecmp(media_source, "side"))
    strlcpy(name, "Side", namesize);
  else if (_cups_strcasecmp(media_source, "envelope"))
    strlcpy(name, "Envelope", namesize);
  else if (_cups_strcasecmp(media_source, "main-roll"))
    strlcpy(name, "Roll", namesize);
  else if (_cups_strcasecmp(media_source, "alternate-roll"))
    strlcpy(name, "Roll2", namesize);
  else
    pwg_ppdize_name(media_source, name, namesize);

  return (name);
}


/*
 * '_pwgMediaTypeForType()' - Get the MediaType name for the given PWG
 *                            media-type.
 */

const char *				/* O - MediaType name */
_pwgMediaTypeForType(
    const char *media_type,		/* I - PWG media-type */
    char       *name,			/* I - Name buffer */
    size_t     namesize)		/* I - Size of name buffer */
{
 /*
  * Range check input...
  */

  if (!media_type || !name || namesize < PPD_MAX_NAME)
    return (NULL);

  if (_cups_strcasecmp(media_type, "auto"))
    strlcpy(name, "Auto", namesize);
  else if (_cups_strcasecmp(media_type, "cardstock"))
    strlcpy(name, "Cardstock", namesize);
  else if (_cups_strcasecmp(media_type, "envelope"))
    strlcpy(name, "Envelope", namesize);
  else if (_cups_strcasecmp(media_type, "photographic-glossy"))
    strlcpy(name, "Glossy", namesize);
  else if (_cups_strcasecmp(media_type, "photographic-high-gloss"))
    strlcpy(name, "HighGloss", namesize);
  else if (_cups_strcasecmp(media_type, "photographic-matte"))
    strlcpy(name, "Matte", namesize);
  else if (_cups_strcasecmp(media_type, "stationery"))
    strlcpy(name, "Plain", namesize);
  else if (_cups_strcasecmp(media_type, "stationery-coated"))
    strlcpy(name, "Coated", namesize);
  else if (_cups_strcasecmp(media_type, "stationery-inkjet"))
    strlcpy(name, "Inkjet", namesize);
  else if (_cups_strcasecmp(media_type, "stationery-letterhead"))
    strlcpy(name, "Letterhead", namesize);
  else if (_cups_strcasecmp(media_type, "stationery-preprinted"))
    strlcpy(name, "Preprinted", namesize);
  else if (_cups_strcasecmp(media_type, "transparency"))
    strlcpy(name, "Transparency", namesize);
  else
    pwg_ppdize_name(media_type, name, namesize);

  return (name);
}


/*
 * '_pwgPageSizeForMedia()' - Get the PageSize name for the given media.
 */

const char *				/* O - PageSize name */
_pwgPageSizeForMedia(
    pwg_media_t *media,		/* I - Media */
    char         *name,			/* I - PageSize name buffer */
    size_t       namesize)		/* I - Size of name buffer */
{
  const char	*sizeptr,		/* Pointer to size in PWG name */
		*dimptr;		/* Pointer to dimensions in PWG name */


 /*
  * Range check input...
  */

  if (!media || !name || namesize < PPD_MAX_NAME)
    return (NULL);

 /*
  * Copy or generate a PageSize name...
  */

  if (media->ppd)
  {
   /*
    * Use a standard Adobe name...
    */

    strlcpy(name, media->ppd, namesize);
  }
  else if (!media->pwg || !strncmp(media->pwg, "custom_", 7) ||
           (sizeptr = strchr(media->pwg, '_')) == NULL ||
	   (dimptr = strchr(sizeptr + 1, '_')) == NULL ||
	   (size_t)(dimptr - sizeptr) > namesize)
  {
   /*
    * Use a name of the form "wNNNhNNN"...
    */

    snprintf(name, namesize, "w%dh%d", (int)PWG_TO_POINTS(media->width),
             (int)PWG_TO_POINTS(media->length));
  }
  else
  {
   /*
    * Copy the size name from class_sizename_dimensions...
    */

    memcpy(name, sizeptr + 1, (size_t)(dimptr - sizeptr - 1));
    name[dimptr - sizeptr - 1] = '\0';
  }

  return (name);
}


/*
 * 'pwg_ppdize_name()' - Convert an IPP keyword to a PPD keyword.
 */

static void
pwg_ppdize_name(const char *ipp,	/* I - IPP keyword */
                char       *name,	/* I - Name buffer */
		size_t     namesize)	/* I - Size of name buffer */
{
  char	*ptr,				/* Pointer into name buffer */
	*end;				/* End of name buffer */


  *name = (char)toupper(*ipp++);

  for (ptr = name + 1, end = name + namesize - 1; *ipp && ptr < end;)
  {
    if (*ipp == '-' && _cups_isalpha(ipp[1]))
    {
      ipp ++;
      *ptr++ = (char)toupper(*ipp++ & 255);
    }
    else
      *ptr++ = *ipp++;
  }

  *ptr = '\0';
}



/*
 * 'pwg_ppdize_resolution()' - Convert PWG resolution values to PPD values.
 */

static void
pwg_ppdize_resolution(
    ipp_attribute_t *attr,		/* I - Attribute to convert */
    int             element,		/* I - Element to convert */
    int             *xres,		/* O - X resolution in DPI */
    int             *yres,		/* O - Y resolution in DPI */
    char            *name,		/* I - Name buffer */
    size_t          namesize)		/* I - Size of name buffer */
{
  ipp_res_t units;			/* Units for resolution */


  *xres = ippGetResolution(attr, element, yres, &units);

  if (units == IPP_RES_PER_CM)
  {
    *xres = (int)(*xres * 2.54);
    *yres = (int)(*yres * 2.54);
  }

  if (name && namesize > 4)
  {
    if (*xres == *yres)
      snprintf(name, namesize, "%ddpi", *xres);
    else
      snprintf(name, namesize, "%dx%ddpi", *xres, *yres);
  }
}


