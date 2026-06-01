/**************************************************************************
*  File: comm.c                                            Part of tbaMUD *
*  Usage: Communication, socket handling, main(), central game loop.      *
*                                                                         *
*  All rights reserved.  See license for complete information.            *
*                                                                         *
*  Copyright (C) 1993, 94 by the Trustees of the Johns Hopkins University *
*  CircleMUD is based on DikuMUD, Copyright (C) 1990, 1991.               *
**************************************************************************/

#include "conf.h"
#include "sysdep.h"

/* Begin conf.h dependent includes */

#if CIRCLE_GNU_LIBC_MEMORY_TRACK
# include <mcheck.h>
#endif

#ifdef CIRCLE_MACINTOSH		/* Includes for the Macintosh */
# define SIGPIPE 13
# define SIGALRM 14
  /* GUSI headers */
# include <sys/ioctl.h>
  /* Codewarrior dependant */
# include <SIOUX.h>
# include <console.h>
#endif

#ifdef CIRCLE_WINDOWS		/* Includes for Win32 */
# ifdef __BORLANDC__
#  include <dir.h>
# else /* MSVC */
#  include <direct.h>
# endif
# include <mmsystem.h>
#endif /* CIRCLE_WINDOWS */

#ifdef CIRCLE_AMIGA		/* Includes for the Amiga */
# include <sys/ioctl.h>
# include <clib/socket_protos.h>
#endif /* CIRCLE_AMIGA */

#ifdef CIRCLE_ACORN		/* Includes for the Acorn (RiscOS) */
# include <socklib.h>
# include <inetlib.h>
# include <sys/ioctl.h>
#endif

#ifdef HAVE_ARPA_TELNET_H
#include <arpa/telnet.h>
#else
#include "telnet.h"
#endif

/* end conf.h dependent includes */

/* Note, most includes for all platforms are in sysdep.h.  The list of
 * files that is included is controlled by conf.h for that platform. */

#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "interpreter.h"
#include "handler.h"
#include "db.h"
#include "house.h"
#include "oasis.h"
#include "genolc.h"
#include "dg_scripts.h"
#include "dg_event.h"
#include "screen.h" /* to support the gemote act type command */
#include "constants.h" /* For mud versions */
#include "boards.h"
#include "act.h"
#include "ban.h"
#include "msgedit.h"
#include "fight.h"
#include "spells.h" /* for affect_update */
#include "modify.h"
#include "quest.h"
#include "ibt.h" /* for free_ibt_lists */
#include "mud_event.h"

#ifndef INVALID_SOCKET
#define INVALID_SOCKET (-1)
#endif

extern time_t motdmod;
extern time_t newsmod;

/* locally defined globals, used externally */
struct descriptor_data *descriptor_list = NULL;   /* master desc list */
int buf_largecount = 0;   /* # of large buffers which exist */
int buf_overflows = 0;    /* # of overflows of output */
int buf_switches = 0;     /* # of switches from small to large buf */
int circle_shutdown = 0;  /* clean shutdown */
int circle_reboot = 0;    /* reboot the game after a shutdown */
int no_specials = 0;      /* Suppress ass. of special routines */
int scheck = 0;           /* for syntax checking mode */
FILE *logfile = NULL;     /* Where to send the log messages. */
unsigned long pulse = 0;  /* number of pulses since game start */
ush_int port;
uv_os_sock_t mother_desc;
int next_tick = SECS_PER_MUD_HOUR;  /* Tick countdown */

/* libuv globals */
uv_loop_t *loop;
uv_tcp_t mother_handle;
uv_timer_t heartbeat_timer;
uv_signal_t sigint_handle;
uv_signal_t sighup_handle;
uv_signal_t sigterm_handle;
uv_signal_t sigusr1_handle;
uv_signal_t sigusr2_handle;

/* static local global variable declarations (current file scope only) */
static struct txt_block *bufpool = 0;  /* pool of large output buffers */
static int max_players = 0;   /* max descriptors available */
static int tics_passed = 0;     /* for extern checkpointing */
static struct timeval null_time; /* zero-valued time structure */
static byte reread_wizlist;   /* signal: SIGUSR1 */
static byte emergency_unban;  /* signal: SIGUSR2 */

static int dg_act_check;         /* toggle for act_trigger */
static bool fCopyOver;          /* Are we booting in copyover mode? */
static char *last_act_message = NULL;

/* libuv callback prototypes */
static void on_new_connection(uv_stream_t *server, int status);
static void alloc_buffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf);
static void on_read(uv_stream_t *client, ssize_t nread, const uv_buf_t *buf);
static void on_write(uv_write_t *req, int status);
static void heartbeat_cb(uv_timer_t *handle);
static void on_signal(uv_signal_t *handle, int signum);

/* static local function prototypes (current file scope only) */
static RETSIGTYPE reread_wizlists(int sig);
/* Appears to be orphaned right now...
static RETSIGTYPE unrestrict_game(int sig);
*/
static RETSIGTYPE reap(int sig);
static RETSIGTYPE checkpointing(int sig);
static RETSIGTYPE hupsig(int sig);
static int get_from_q(struct txt_q *queue, char *dest, int *aliased);
static void init_game(ush_int port);
static void signal_setup(void);
static socket_t init_socket(ush_int port);
static int get_max_players(void);
static int process_output(struct descriptor_data *t);
static int process_input(struct descriptor_data *t);
static void flush_queues(struct descriptor_data *d);
static int perform_subst(struct descriptor_data *t, char *orig, char *subst);
static void record_usage(void);
static char *make_prompt(struct descriptor_data *point);
static void check_idle_passwords(void);
static void init_descriptor (struct descriptor_data *newd);

static void free_bufpool(void);
static void setup_log(const char *filename, int fd);
static int open_logfile(const char *filename, FILE *stderr_fp);
#if defined(POSIX)
static sigfunc *my_signal(int signo, sigfunc *func);
#endif

static void msdp_update(void); /* KaVir plugin*/

/* libuv callbacks */

static void alloc_buffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf)
{
  *buf = uv_buf_init((char *)malloc(suggested_size), suggested_size);
}

static void on_read(uv_stream_t *client, ssize_t nread, const uv_buf_t *buf)
{
  struct descriptor_data *d = (struct descriptor_data *)client->data;

  if (nread > 0) {
    /* Find where to append in the buffer */
    int current_len = strlen(d->inbuf);
    
    /* Limit nread to available space in d->inbuf */
    if (current_len + nread >= MAX_RAW_INPUT_LENGTH) {
      log("WARNING: input overflow on descriptor %d", d->desc_num);
      nread = MAX_RAW_INPUT_LENGTH - current_len - 1;
    }

    if (nread > 0) {
      char *append_point = d->inbuf + current_len;
      
      /* Since we have received atleast 1 byte of data from the socket, lets run it through
       * ProtocolInput() and rip out anything that is Out Of Band.
       * ProtocolInput appends to d->inbuf. 
       * We pass d->inbuf as apOut, and it returns bytes added. */ 
      ssize_t added = ProtocolInput(d, buf->base, nread, d->inbuf);
      
      /* Terminate the buffer at the new end point */
      if (added >= 0) {
        *(append_point + added) = '\0';
      }
    }
  } else if (nread < 0) {
    if (nread != UV_EOF)
      log("Read error %s", uv_err_name(nread));
    STATE(d) = CON_CLOSE;
  }

  if (buf->base)
    free(buf->base);
}

static void on_reject_write(uv_write_t *req, int status)
{
  uv_stream_t *stream = req->handle;
  uv_close((uv_handle_t *)stream, (uv_close_cb)free);
  if (req->data)
    free(req->data);
  free(req);
}

static void on_new_connection(uv_stream_t *server, int status)
{
  if (status < 0) {
    log("New connection error %s", uv_strerror(status));
    return;
  }

  int sockets_connected = 0;
  struct descriptor_data *d;
  for (d = descriptor_list; d; d = d->next)
    sockets_connected++;

  struct descriptor_data *newd;
  CREATE(newd, struct descriptor_data, 1);
  
  uv_tcp_init(loop, &newd->handle);
  if (uv_accept(server, (uv_stream_t *)&newd->handle) == 0) {
    newd->handle.data = newd;

    /* Get the raw fd for legacy code compatibility */
    uv_os_fd_t fd;
    uv_fileno((const uv_handle_t *)&newd->handle, &fd);

    if (sockets_connected >= CONFIG_MAX_PLAYING) {
      const char *msg = "Sorry, the game is full right now... please try again later!\r\n";
      uv_buf_t buf = uv_buf_init(strdup(msg), strlen(msg));
      uv_write_t *req = (uv_write_t *)malloc(sizeof(uv_write_t));
      req->data = buf.base;
      uv_write(req, (uv_stream_t *)&newd->handle, &buf, 1, on_reject_write);
      /* descriptor_data 'newd' was created but not used, free it */
      free(newd);
      return;
    }

    /* Initialize descriptor data */
    struct sockaddr_storage peer;
    int len = sizeof(peer);
    uv_tcp_getpeername(&newd->handle, (struct sockaddr *)&peer, &len);
    
    if (peer.ss_family == AF_INET) {
      uv_ip4_name((struct sockaddr_in *)&peer, newd->host, HOST_LENGTH);
    } else if (peer.ss_family == AF_INET6) {
      uv_ip6_name((struct sockaddr_in6 *)&peer, newd->host, HOST_LENGTH);
    } else {
      strcpy(newd->host, "unknown");
    }
    newd->host[HOST_LENGTH] = '\0';

    if (isbanned(newd->host) == BAN_ALL) {
      log("Connection attempt denied from banned host %s", newd->host);
      uv_close((uv_handle_t *)&newd->handle, (uv_close_cb)free);
      return;
    }

    init_descriptor(newd);
    
    newd->next = descriptor_list;
    descriptor_list = newd;

    uv_read_start((uv_stream_t *)&newd->handle, alloc_buffer, on_read);
    
    if (CONFIG_PROTOCOL_NEGOTIATION) {
      NEW_EVENT(ePROTOCOLS, newd, NULL, 1.5 * PASSES_PER_SEC);
      write_to_output(newd, "Attempting to Detect Client, Please Wait...\r\n");
      ProtocolNegotiate(newd);
    } else {
      int greetsize = strlen(GREETINGS);
      write_to_output(newd, "%s", ProtocolOutput(newd, GREETINGS, &greetsize));
    }
  } else {
    uv_close((uv_handle_t *)&newd->handle, (uv_close_cb)free);
  }
}

static void on_write(uv_write_t *req, int status)
{
  if (req->data)
    free(req->data);
  if (status) {
    log("Write error %s", uv_strerror(status));
  }
  free(req);
}

static void on_descriptor_close(uv_handle_t *handle)
{
  struct descriptor_data *d = (struct descriptor_data *)handle->data;
  free(d);
}

/* write_to_descriptor takes a descriptor, and text to write to the descriptor.
 * It uses libuv to send the data asynchronously. Returns:
 * >=0  If the write was successfully queued.
 *  -1  If an error was encountered. */
int write_to_descriptor(struct descriptor_data *d, const char *txt)
{
  size_t len = strlen(txt);

  if (len == 0 || !d)
    return 0;

  uv_buf_t buf = uv_buf_init(strdup(txt), len);
  uv_write_t *req = (uv_write_t *)malloc(sizeof(uv_write_t));
  req->data = buf.base;
  if (uv_write(req, (uv_stream_t *)&d->handle, &buf, 1, on_write) != 0) {
    free(buf.base);
    free(req);
    return -1;
  }
  return len;
}

static void heartbeat_cb(uv_timer_t *handle)
{
  struct descriptor_data *d, *next_d;
  char comm[MAX_INPUT_LENGTH];
  int aliased;

  /* 1. World update */
  heartbeat(++pulse);

  /* 2. Process commands (one per pulse per player) */
  for (d = descriptor_list; d; d = next_d) {
    next_d = d->next;

    if (STATE(d) == CON_CLOSE || STATE(d) == CON_DISCONNECT) {
      close_socket(d);
      continue;
    }

    /* Input processing */
    process_input(d);

    if (d->character) {
      GET_WAIT_STATE(d->character) -= (GET_WAIT_STATE(d->character) > 0);
      if (GET_WAIT_STATE(d->character))
        continue;
    }

    if (!get_from_q(&d->input, comm, &aliased))
      continue;

    if (d->character) {
      d->character->char_specials.timer = 0;
      if (STATE(d) == CON_PLAYING && GET_WAS_IN(d->character) != NOWHERE) {
        if (IN_ROOM(d->character) != NOWHERE)
          char_from_room(d->character);
        char_to_room(d->character, GET_WAS_IN(d->character));
        GET_WAS_IN(d->character) = NOWHERE;
        act("$n has returned.", TRUE, d->character, 0, 0, TO_ROOM);
      }
      GET_WAIT_STATE(d->character) = 1;
    }
    d->has_prompt = FALSE;

    if (d->showstr_count)
      show_string(d, comm);
    else if (d->str)
      string_add(d, comm);
    else if (STATE(d) != CON_PLAYING)
      nanny(d, comm);
    else {
      if (aliased)
        d->has_prompt = TRUE;
      else if (perform_alias(d, comm, sizeof(comm)))
        get_from_q(&d->input, comm, &aliased);
      command_interpreter(d->character, comm);
    }
  }

  /* 3. Output and Prompts */
  for (d = descriptor_list; d; d = next_d) {
    next_d = d->next;
    
    if (*(d->output)) {
      if (process_output(d) < 0) {
        close_socket(d);
        continue;
      }
      d->has_prompt = 1;
    }

    if (!d->has_prompt) {
      write_to_descriptor(d, make_prompt(d));
      d->has_prompt = TRUE;
    }
  }

  if (circle_shutdown) {
    uv_stop(loop);
  }
}

static void on_signal(uv_signal_t *handle, int signum)
{
  switch (signum) {
    case SIGINT:
    case SIGTERM:
      log("Received SIGINT/SIGTERM, shutting down...");
      circle_shutdown = 1;
      uv_stop(loop);
      break;
    case SIGHUP:
      log("Received SIGHUP, rebooting...");
      circle_shutdown = 1;
      circle_reboot = 1;
      uv_stop(loop);
      break;
#ifdef SIGUSR1
    case SIGUSR1:
      reread_wizlist = TRUE;
      break;
#endif
#ifdef SIGUSR2
    case SIGUSR2:
      emergency_unban = TRUE;
      break;
#endif
  }
}

/*  main game loop and related stuff */
int main(int argc, char **argv)
{
  int pos = 1;
  const char *dir;

#ifdef MEMORY_DEBUG
  zmalloc_init();
#endif

#if CIRCLE_GNU_LIBC_MEMORY_TRACK
  mtrace();	/* This must come before any use of malloc(). */
#endif

#ifdef CIRCLE_MACINTOSH
  /* ccommand() calls the command line/io redirection dialog box from
   * Codewarriors's SIOUX library. */
  argc = ccommand(&argv);
  /* Initialize the GUSI library calls.  */
  GUSIDefaultSetup();
#endif

  /* Load the game configuration. We must load BEFORE we use any of the
   * constants stored in constants.c.  Otherwise, there will be no variables
   * set to set the rest of the vars to, which will mean trouble --> Mythran */
  CONFIG_CONFFILE = NULL;
  while ((pos < argc) && (*(argv[pos]) == '-')) {
    if (*(argv[pos] + 1) == 'f') {
      if (*(argv[pos] + 2))
	CONFIG_CONFFILE = argv[pos] + 2;
      else if (++pos < argc)
	CONFIG_CONFFILE = argv[pos];
      else {
	puts("SYSERR: File name to read from expected after option -f.");
	exit(1);
      }
    }
    pos++;
  }
  pos = 1;

  if (!CONFIG_CONFFILE)
    CONFIG_CONFFILE = strdup(CONFIG_FILE);

  load_config();

  port = CONFIG_DFLT_PORT;
  dir = CONFIG_DFLT_DIR;

  while ((pos < argc) && (*(argv[pos]) == '-')) {
    switch (*(argv[pos] + 1)) {
    case 'f':
      if (! *(argv[pos] + 2))
	++pos;
      break;
    case 'o':
      if (*(argv[pos] + 2))
	CONFIG_LOGNAME = argv[pos] + 2;
      else if (++pos < argc)
	CONFIG_LOGNAME = argv[pos];
      else {
	puts("SYSERR: File name to log to expected after option -o.");
	exit(1);
      }
      break;
    case 'C': /* -C<socket number> - recover from copyover, this is the control socket */
       fCopyOver = TRUE;
       mother_desc = atoi(argv[pos]+2);
      break;
    case 'd':
      if (*(argv[pos] + 2))
	dir = argv[pos] + 2;
      else if (++pos < argc)
	dir = argv[pos];
      else {
	puts("SYSERR: Directory arg expected after option -d.");
	exit(1);
      }
      break;
    case 'm':
      mini_mud = 1;
      no_rent_check = 1;
      puts("Running in minimized mode & with no rent check.");
      break;
    case 'c':
      scheck = 1;
      puts("Syntax check mode enabled.");
      break;
    case 'q':
      no_rent_check = 1;
      puts("Quick boot mode -- rent check supressed.");
      break;
    case 'r':
      circle_restrict = 1;
      puts("Restricting game -- no new players allowed.");
      break;
    case 's':
      no_specials = 1;
      puts("Suppressing assignment of special routines.");
      break;
    case 'h':
      /* From: Anil Mahajan. Do NOT use -C, this is the copyover mode and
       * without the proper copyover.dat file, the game will go nuts! */
      printf("Usage: %s [-c] [-m] [-q] [-r] [-s] [-d pathname] [port #]\n"
              "  -c             Enable syntax check mode.\n"
              "  -d <directory> Specify library directory (defaults to 'lib').\n"
              "  -h             Print this command line argument help.\n"
              "  -m             Start in mini-MUD mode.\n"
	      "  -f<file>       Use <file> for configuration.\n"
	      "  -o <file>      Write log to <file> instead of stderr.\n"
              "  -q             Quick boot (doesn't scan rent for object limits)\n"
              "  -r             Restrict MUD -- no new players allowed.\n"
              "  -s             Suppress special procedure assignments.\n"
              " Note:		These arguments are 'CaSe SeNsItIvE!!!'\n",
		 argv[0]
      );
      exit(0);
    default:
      printf("SYSERR: Unknown option -%c in argument string.\n", *(argv[pos] + 1));
      break;
    }
    pos++;
  }

  if (pos < argc) {
    if (!isdigit(*argv[pos])) {
      printf("Usage: %s [-c] [-m] [-q] [-r] [-s] [-d pathname] [port #]\n", argv[0]);
      exit(1);
    } else if ((port = atoi(argv[pos])) <= 1024) {
      printf("SYSERR: Illegal port number %d.\n", port);
      exit(1);
    }
  }

  /* All arguments have been parsed, try to open log file. */
  setup_log(CONFIG_LOGNAME, STDERR_FILENO);

  /* Moved here to distinguish command line options and to show up
   * in the log if stderr is redirected to a file. */
  log("Loading configuration.");
  log("%s", tbamud_version);

  if (chdir(dir) < 0) {
    perror("SYSERR: Fatal error changing to data directory");
    exit(1);
  }
  log("Using %s as data directory.", dir);

  if (scheck)
    boot_world();
  else {
    log("Running game on port %d.", port);
    init_game(port);
  }

  log("Clearing game world.");
  destroy_db();

  if (!scheck) {
    log("Clearing other memory.");
    free_bufpool();         /* comm.c */
    free_player_index();    /* players.c */
    free_messages();        /* fight.c */
    free_text_files();      /* db.c */
    board_clear_all();      /* boards.c */
    free(cmd_sort_info);    /* act.informative.c */
    free_command_list();    /* act.informative.c */
    free_social_messages(); /* act.social.c */
    free_help_table();      /* db.c */
    free_invalid_list();    /* ban.c */
    free_save_list();       /* genolc.c */
    free_strings(&config_info, OASIS_CFG); /* oasis_delete.c */
    free_ibt_lists();       /* ibt.c */
    free_recent_players();  /* act.informative.c */
    free_list(world_events); /* free up our global lists */
    free_list(global_lists);
  }

  if (last_act_message)
    free(last_act_message);

  /* probably should free the entire config here.. */
  free(CONFIG_CONFFILE);
  
  log("Done.");

#ifdef MEMORY_DEBUG
  zmalloc_check();
#endif

  return (0);
}

/* Reload players after a copyover */
void copyover_recover()
{
  struct descriptor_data *d;
  FILE *fp;
  char host[1024], guiopt[1024];
  int desc, i, player_i;
  bool fOld;
  char name[MAX_INPUT_LENGTH];
  long pref;

  log ("Copyover recovery initiated");

  fp = fopen (COPYOVER_FILE, "r");
  /* there are some descriptors open which will hang forever then ? */
  if (!fp) {
    perror ("copyover_recover:fopen");
    log ("Copyover file not found. Exitting.\n\r");
    exit (1);
  }

  /* In case something crashes - doesn't prevent reading  */
  unlink (COPYOVER_FILE);

  /* read boot_time - first line in file */
  i = fscanf(fp, "%ld\n", (long *)&boot_time);
  
  if (i != 1) 
    log("SYSERR: Error reading boot time.");

  for (;;) {
    fOld = TRUE;
    if (fscanf(fp, "%d %ld %s %s %s\n", &desc, &pref, name, host, guiopt) != 5) {
      if(!feof(fp)) {
        if(ferror(fp))
          log("SYSERR: error reading copyover file %s: %s", COPYOVER_FILE, strerror(errno));
        else if(!feof(fp))
          log("SYSERR: could not scan line in copyover file %s.", COPYOVER_FILE);
        exit(1);
      }
    }

    if (desc == -1)
      break;

    /* create a new descriptor */
    CREATE (d, struct descriptor_data, 1);
    memset ((char *) d, 0, sizeof (struct descriptor_data));
    
    /* Initialize libuv handle from raw descriptor */
    uv_tcp_init(loop, &d->handle);
    uv_tcp_open(&d->handle, (uv_os_sock_t)desc);
    d->handle.data = d;
    uv_read_start((uv_stream_t *)&d->handle, alloc_buffer, on_read);

    init_descriptor (d); /* set up various stuff */

    /* Write something, and check if it goes error-free */
    if (write_to_descriptor (d, "\n\rRestoring from copyover...\n\r") < 0) {
      close_socket (d);
      continue;
    }

    strcpy(d->host, host);
    d->next = descriptor_list;
    descriptor_list = d;

    d->connected = CON_CLOSE;

    CopyoverSet(d,guiopt);

    /* Now, find the pfile */
    CREATE(d->character, struct char_data, 1);
    clear_char(d->character);
    CREATE(d->character->player_specials, struct player_special_data, 1);
    
    new_mobile_data(d->character);
    
    d->character->desc = d;

    if ((player_i = load_char(name, d->character)) >= 0) {
      GET_PFILEPOS(d->character) = player_i;
      if (!PLR_FLAGGED(d->character, PLR_DELETED)) {
        REMOVE_BIT_AR(PLR_FLAGS(d->character), PLR_WRITING);
        REMOVE_BIT_AR(PLR_FLAGS(d->character), PLR_MAILING);
        REMOVE_BIT_AR(PLR_FLAGS(d->character), PLR_CRYO);
      } else
        fOld = FALSE;
    } else
      fOld = FALSE;

    /* Player file not found?! */
    if (!fOld) {
      write_to_descriptor (d, "\n\rSomehow, your character was lost in the copyover. Sorry.\n\r");
      close_socket (d);
    } else {
      write_to_descriptor (d, "\n\rCopyover recovery complete.\n\r");
      GET_PREF(d->character) = pref;
    
      enter_player_game(d);

      /* Clear their load room if it's not persistant. */
      if (!PLR_FLAGGED(d->character, PLR_LOADROOM))
        GET_LOADROOM(d->character) = NOWHERE;

      d->connected = CON_PLAYING;
      look_at_room(d->character, 0);

      /* Add to the list of 'recent' players (since last reboot) with copyover flag */
      if (AddRecentPlayer(GET_NAME(d->character), d->host, FALSE, TRUE) == FALSE)
      {
        mudlog(BRF, MAX(LVL_IMMORT, GET_INVIS_LEV(d->character)), TRUE, "Failure to AddRecentPlayer (returned FALSE).");
      }
    }
  }
  fclose (fp);
}

/* Init libuv and game structures */
static void init_game(ush_int local_port)
{
  /* We don't want to restart if we crash before we get up. */
  touch(KILLSCRIPT_FILE);

  circle_srandom(time(0));

  log("Finding player limit.");
  max_players = get_max_players();

  /* Initialize libuv loop */
  loop = uv_default_loop();

  /* Set up signal handling */
  uv_signal_init(loop, &sigint_handle);
  uv_signal_start(&sigint_handle, on_signal, SIGINT);
  uv_signal_init(loop, &sigterm_handle);
  uv_signal_start(&sigterm_handle, on_signal, SIGTERM);
#if defined(CIRCLE_UNIX)
  uv_signal_init(loop, &sighup_handle);
  uv_signal_start(&sighup_handle, on_signal, SIGHUP);
  uv_signal_init(loop, &sigusr1_handle);
  uv_signal_start(&sigusr1_handle, on_signal, SIGUSR1);
  uv_signal_init(loop, &sigusr2_handle);
  uv_signal_start(&sigusr2_handle, on_signal, SIGUSR2);
#endif

  /* If copyover mother_desc is already set up */
  if (!fCopyOver) {
     log ("Opening mother connection.");
     mother_desc = init_socket (local_port);
  } else {
    /* Need to adapt copyover socket to libuv handle */
    uv_tcp_init(loop, &mother_handle);
    uv_tcp_open(&mother_handle, mother_desc);
    uv_listen((uv_stream_t *)&mother_handle, 5, on_new_connection);
  }

  event_init();

  /* set up hash table for find_char() */
  init_lookup_table();

  boot_db();

  /* If we made it this far, we will be able to restart without problem. */
  remove(KILLSCRIPT_FILE);

  if (fCopyOver) /* reload players */
    copyover_recover();

  /* Initialize heartbeat timer */
  uv_timer_init(loop, &heartbeat_timer);
  uv_timer_start(&heartbeat_timer, heartbeat_cb, 0, OPT_USEC / 1000);

  log("Entering game loop.");

  game_loop();

  Crash_save_all();

  log("Closing all sockets.");
  while (descriptor_list)
    close_socket(descriptor_list);

  uv_close((uv_handle_t *)&mother_handle, NULL);
  uv_run(loop, UV_RUN_DEFAULT); /* Run one more time to let closes finish */

  if (circle_reboot != 2)
    save_all();

  log("Saving current MUD time.");
  save_mud_time(&time_info);

  if (circle_reboot) {
    log("Rebooting.");
    exit(52);			/* what's so great about HHGTTG, anyhow? */
  }
  log("Normal termination of game.");
}

/* init_socket sets up the mother descriptor - creates the socket, sets
 * its options up, binds it, and listens. */
static socket_t init_socket(ush_int local_port)
{
  struct sockaddr_in sa;
  int r;

  uv_tcp_init(loop, &mother_handle);

  if (uv_ip4_addr(CONFIG_DFLT_IP ?: "0.0.0.0", local_port, &sa)) {
    log("SYSERR: DFLT_IP of %s appears to be an invalid IP address", CONFIG_DFLT_IP);
    uv_ip4_addr("0.0.0.0", local_port, &sa);
  }
  /* Put the address that we've finally decided on into the logs */
  if (sa.sin_addr.s_addr == htonl(INADDR_ANY))
      log("Binding to all IP interfaces on this host.");
  else
      log("Binding only to IP address %s", inet_ntoa(sa.sin_addr));

  uv_tcp_bind(&mother_handle, (const struct sockaddr*)&sa, 0);
  
  if ((r = uv_listen((uv_stream_t *)&mother_handle, 5, on_new_connection))) {
    log("uv_listen error %s", uv_strerror(r));
    exit(1);
  }

  /* Return the raw fd for legacy code that still expects mother_desc */
  /* uv_fileno returns the fd if it's open */
  uv_os_fd_t fd;
  uv_fileno((const uv_handle_t *)&mother_handle, &fd);
  return (socket_t)fd;
}

static int get_max_players(void)
{
#ifndef CIRCLE_UNIX
  return (CONFIG_MAX_PLAYING);
#else

  int max_descs = 0;
  const char *method;

/* First, we'll try using getrlimit/setrlimit.  This will probably work
 * on most systems.  HAS_RLIMIT is defined in sysdep.h. */
#ifdef HAS_RLIMIT
  {
    struct rlimit limit;

    /* find the limit of file descs */
    method = "rlimit";
    if (getrlimit(RLIMIT_NOFILE, &limit) < 0) {
      perror("SYSERR: calling getrlimit");
      exit(1);
    }

    /* set the current to the maximum */
    limit.rlim_cur = limit.rlim_max;
    if (setrlimit(RLIMIT_NOFILE, &limit) < 0) {
#if defined(__APPLE__) && defined(RLIMIT_NOFILE) && defined(OPEN_MAX)
        /* On OS X, setting rlim_cur above OPEN_MAX fails, so try again
         * with OPEN_MAX. */
      limit.rlim_cur = OPEN_MAX;
#endif
      if (setrlimit(RLIMIT_NOFILE, &limit) < 0) {
        perror("SYSERR: calling setrlimit");
        exit(1);
      }
    }
#ifdef RLIM_INFINITY
    if (limit.rlim_max == RLIM_INFINITY)
      max_descs = CONFIG_MAX_PLAYING + NUM_RESERVED_DESCS;
    else
      max_descs = MIN(CONFIG_MAX_PLAYING + NUM_RESERVED_DESCS, limit.rlim_max);
#else
    max_descs = MIN(CONFIG_MAX_PLAYING + NUM_RESERVED_DESCS, limit.rlim_max);
#endif
  }

#elif defined (OPEN_MAX) || defined(FOPEN_MAX)
#if !defined(OPEN_MAX)
#define OPEN_MAX FOPEN_MAX
#endif
  method = "OPEN_MAX";
  max_descs = OPEN_MAX;		/* Uh oh.. rlimit didn't work, but we have
				 * OPEN_MAX */
#elif defined (_SC_OPEN_MAX)
  /* Okay, you don't have getrlimit() and you don't have OPEN_MAX.  Time to
   * try the POSIX sysconf() function.  (See Stevens' _Advanced Programming
   * in the UNIX Environment_). */
  method = "POSIX sysconf";
  errno = 0;
  if ((max_descs = sysconf(_SC_OPEN_MAX)) < 0) {
    if (errno == 0)
      max_descs = CONFIG_MAX_PLAYING + NUM_RESERVED_DESCS;
    else {
      perror("SYSERR: Error calling sysconf");
      exit(1);
    }
  }
#else
  /* if everything has failed, we'll just take a guess */
  method = "random guess";
  max_descs = CONFIG_MAX_PLAYING + NUM_RESERVED_DESCS;
#endif

  /* now calculate max _players_ based on max descs */
  max_descs = MIN(CONFIG_MAX_PLAYING, max_descs - NUM_RESERVED_DESCS);

  if (max_descs <= 0) {
    log("SYSERR: Non-positive max player limit!  (Set at %d using %s).",
	    max_descs, method);
    exit(1);
  }
  log("   Setting player limit to %d using %s.", max_descs, method);
  return (max_descs);
#endif /* CIRCLE_UNIX */
}

void game_loop(void)
{
  log("Entering libuv loop.");
  uv_run(loop, UV_RUN_DEFAULT);
  log("Exiting libuv loop.");
}

void heartbeat(int heart_pulse)
{
  static int mins_since_crashsave = 0;

  event_process();

  if (!(heart_pulse % PULSE_DG_SCRIPT))
    script_trigger_check();

  if (!(heart_pulse % PASSES_PER_SEC)) {    /* EVERY second */
    msdp_update();
    next_tick--;
  }

  if (!(heart_pulse % PULSE_ZONE))
    zone_update();

  if (!(heart_pulse % PULSE_IDLEPWD))		/* 15 seconds */
    check_idle_passwords();

  if (!(heart_pulse % PULSE_MOBILE))
    mobile_activity();

  if (!(heart_pulse % PULSE_VIOLENCE))
    perform_violence();

  if (!(heart_pulse % (SECS_PER_MUD_HOUR * PASSES_PER_SEC))) {  /* Tick ! */
    next_tick = SECS_PER_MUD_HOUR;  /* Reset tick coundown */
    weather_and_time(1);
    check_time_triggers();
    affect_update();
    point_update();
    check_timed_quests();
  }

  if (CONFIG_AUTO_SAVE && !(heart_pulse % PULSE_AUTOSAVE)) {	/* 1 minute */
    if (++mins_since_crashsave >= CONFIG_AUTOSAVE_TIME) {
      mins_since_crashsave = 0;
      Crash_save_all();
      House_save_all();
    }
  }

  if (!(heart_pulse % PULSE_USAGE))
    record_usage();

  if (!(heart_pulse % PULSE_TIMESAVE))
  save_mud_time(&time_info);

  /* Every pulse! Don't want them to stink the place up... */
  extract_pending_chars();
}

static void record_usage(void)
{
  int sockets_connected = 0, sockets_playing = 0;
  struct descriptor_data *d;

  for (d = descriptor_list; d; d = d->next) {
    sockets_connected++;
    if (IS_PLAYING(d))
      sockets_playing++;
  }

  log("nusage: %-3d sockets connected, %-3d sockets playing",
	  sockets_connected, sockets_playing);

#ifdef RUSAGE	/* Not RUSAGE_SELF because it doesn't guarantee prototype. */
  {
    struct rusage ru;

    getrusage(RUSAGE_SELF, &ru);
    log("rusage: user time: %ld sec, system time: %ld sec, max res size: %ld",
	    ru.ru_utime.tv_sec, ru.ru_stime.tv_sec, ru.ru_maxrss);
  }
#endif
}

/* Turn off echoing (specific to telnet client) */
void echo_off(struct descriptor_data *d)
{
  char off_string[] =
  {
    (char) IAC,
    (char) WILL,
    (char) TELOPT_ECHO,
    (char) 0,
  };

  write_to_output(d, "%s", off_string);
}

/* Turn on echoing (specific to telnet client) */
void echo_on(struct descriptor_data *d)
{
  char on_string[] =
  {
    (char) IAC,
    (char) WONT,
    (char) TELOPT_ECHO,
    (char) 0
  };

  write_to_output(d, "%s", on_string);
}

static char *make_prompt(struct descriptor_data *d)
{
  static char prompt[MAX_PROMPT_LENGTH];

  /* Note, prompt is truncated at MAX_PROMPT_LENGTH chars (structs.h) */

  if (d->showstr_count)
    snprintf(prompt, sizeof(prompt),
      "[ Return to continue, (q)uit, (r)efresh, (b)ack, or page number (%d/%d) ]",
      d->showstr_page, d->showstr_count);
  else if (d->str)
    strcpy(prompt, "] ");	/* strcpy: OK (for 'MAX_PROMPT_LENGTH >= 3') */
  else if (STATE(d) == CON_PLAYING && !IS_NPC(d->character)) {
    int count;
    size_t len = 0;

    *prompt = '\0';

    if (GET_INVIS_LEV(d->character) && len < sizeof(prompt)) {
      count = snprintf(prompt + len, sizeof(prompt) - len, "i%d ", GET_INVIS_LEV(d->character));
      if (count >= 0)
        len += count;
    }
    /* show only when below 25% */
    if (PRF_FLAGGED(d->character, PRF_DISPAUTO) && len < sizeof(prompt)) {
      struct char_data *ch = d->character;
      if (GET_HIT(ch) << 2 < GET_MAX_HIT(ch) ) {
        count = snprintf(prompt + len, sizeof(prompt) - len, "%dH ", GET_HIT(ch));
        if (count >= 0)
          len += count;
      }
      if (GET_MANA(ch) << 2 < GET_MAX_MANA(ch) && len < sizeof(prompt)) {
        count = snprintf(prompt + len, sizeof(prompt) - len, "%dM ", GET_MANA(ch));
        if (count >= 0)
          len += count;
      }
      if (GET_MOVE(ch) << 2 < GET_MAX_MOVE(ch) && len < sizeof(prompt)) {
        count = snprintf(prompt + len, sizeof(prompt) - len, "%dV ", GET_MOVE(ch));
        if (count >= 0)
          len += count;
      }
    } else { /* not auto prompt */
      if (PRF_FLAGGED(d->character, PRF_DISPHP) && len < sizeof(prompt)) {
        count = snprintf(prompt + len, sizeof(prompt) - len, "%dH ", GET_HIT(d->character));
        if (count >= 0)
          len += count;
      }

      if (PRF_FLAGGED(d->character, PRF_DISPMANA) && len < sizeof(prompt)) {
        count = snprintf(prompt + len, sizeof(prompt) - len, "%dM ", GET_MANA(d->character));
        if (count >= 0)
          len += count;
      }

      if (PRF_FLAGGED(d->character, PRF_DISPMOVE) && len < sizeof(prompt)) {
        count = snprintf(prompt + len, sizeof(prompt) - len, "%dV ", GET_MOVE(d->character));
        if (count >= 0)
          len += count;
      }
    }

    if (PRF_FLAGGED(d->character, PRF_BUILDWALK) && len < sizeof(prompt)) {
      count = snprintf(prompt + len, sizeof(prompt) - len, "BUILDWALKING ");
      if (count >= 0)
        len += count;
    }

    if (PRF_FLAGGED(d->character, PRF_AFK) && len < sizeof(prompt)) {
      count = snprintf(prompt + len, sizeof(prompt) - len, "AFK ");
      if (count >= 0)
        len += count;
    }

     if (GET_LAST_NEWS(d->character) < newsmod)
     {
       count = snprintf(prompt + len, sizeof(prompt) - len, "(news) ");
       if (count >= 0)
         len += count;
     }

     if (GET_LAST_MOTD(d->character) < motdmod)
     {
       count = snprintf(prompt + len, sizeof(prompt) - len, "(motd) ");
       if (count >= 0)
         len += count;
     }

    if (len < sizeof(prompt))
      strncat(prompt, "> ", sizeof(prompt) - len - 1);	/* strncat: OK */
  } else if (STATE(d) == CON_PLAYING && IS_NPC(d->character))
    snprintf(prompt, sizeof(prompt), "%s> ", GET_NAME(d->character));
  else
    *prompt = '\0';

  return (prompt);
}

/* NOTE: 'txt' must be at most MAX_INPUT_LENGTH big. */
void write_to_q(const char *txt, struct txt_q *queue, int aliased)
{
  struct txt_block *newt;

  CREATE(newt, struct txt_block, 1);
  newt->text = strdup(txt);
  newt->aliased = aliased;

  /* queue empty? */
  if (!queue->head) {
    newt->next = NULL;
    queue->head = queue->tail = newt;
  } else {
    queue->tail->next = newt;
    queue->tail = newt;
    newt->next = NULL;
  }
}

/* NOTE: 'dest' must be at least MAX_INPUT_LENGTH big. */
static int get_from_q(struct txt_q *queue, char *dest, int *aliased)
{
  struct txt_block *tmp;

  /* queue empty? */
  if (!queue->head)
    return (0);

  strcpy(dest, queue->head->text);	/* strcpy: OK (mutual MAX_INPUT_LENGTH) */
  *aliased = queue->head->aliased;

  tmp = queue->head;
  queue->head = queue->head->next;
  free(tmp->text);
  free(tmp);

  return (1);
}

/* Empty the queues before closing connection */
static void flush_queues(struct descriptor_data *d)
{
  if (d->large_outbuf) {
    d->large_outbuf->next = bufpool;
    bufpool = d->large_outbuf;
  }
  while (d->input.head) {
    struct txt_block *tmp = d->input.head;
    d->input.head = d->input.head->next;
    free(tmp->text);
    free(tmp);
  }
}

/* Add a new string to a player's output queue. For outside use. */
size_t write_to_output(struct descriptor_data *t, const char *txt, ...)
{
  va_list args;
  size_t left;

  va_start(args, txt);
  left = vwrite_to_output(t, txt, args);
  va_end(args);

  return left;
}

/* Add a new string to a player's output queue. */
size_t vwrite_to_output(struct descriptor_data *t, const char *format, va_list args)
{
  const char *text_overflow = "\r\nOVERFLOW\r\n";
  static char txt[MAX_STRING_LENGTH];
  size_t wantsize;
  int size;

  /* if we're in the overflow state already, ignore this new output */
  if (t->bufspace == 0)
    return (0);

  wantsize = size = vsnprintf(txt, sizeof(txt), format, args);

  strcpy(txt, ProtocolOutput( t, txt, (int*)&wantsize )); /* <--- Add this line */
  size = wantsize;                    /* <--- Add this line */
  if ( t->pProtocol->WriteOOB > 0 )   /* <--- Add this line */
    --t->pProtocol->WriteOOB;         /* <--- Add this line */

  /* If exceeding the size of the buffer, truncate it for the overflow message */
  if (size < 0 || wantsize >= sizeof(txt)) {
    size = sizeof(txt) - 1;
    strcpy(txt + size - strlen(text_overflow), text_overflow);	/* strcpy: OK */
  }

  /* If the text is too big to fit into even a large buffer, truncate
   * the new text to make it fit.  (This will switch to the overflow
   * state automatically because t->bufspace will end up 0.) */
  if (size + t->bufptr + 1 > LARGE_BUFSIZE) {
    size = LARGE_BUFSIZE - t->bufptr - 1;
    txt[size] = '\0';
    buf_overflows++;
  }

  /* If we have enough space, just write to buffer and that's it! If the
   * text just barely fits, then it's switched to a large buffer instead. */
  if (t->bufspace > size) {
    strcpy(t->output + t->bufptr, txt);	/* strcpy: OK (size checked above) */
    t->bufspace -= size;
    t->bufptr += size;
    return (t->bufspace);
  }

  buf_switches++;

  /* if the pool has a buffer in it, grab it */
  if (bufpool != NULL) {
    t->large_outbuf = bufpool;
    bufpool = bufpool->next;
  } else {			/* else create a new one */
    CREATE(t->large_outbuf, struct txt_block, 1);
    CREATE(t->large_outbuf->text, char, LARGE_BUFSIZE);
    buf_largecount++;
  }

  strcpy(t->large_outbuf->text, t->output);	/* strcpy: OK (size checked previously) */
  t->output = t->large_outbuf->text;	/* make big buffer primary */
  strcat(t->output, txt);	/* strcat: OK (size checked) */

  /* set the pointer for the next write */
  t->bufptr = strlen(t->output);

  /* calculate how much space is left in the buffer */
  t->bufspace = LARGE_BUFSIZE - 1 - t->bufptr;

  return (t->bufspace);
}

static void free_bufpool(void)
{
  struct txt_block *tmp;

  while (bufpool) {
    tmp = bufpool->next;
    if (bufpool->text)
      free(bufpool->text);
    free(bufpool);
    bufpool = tmp;
  }
}

static void init_descriptor (struct descriptor_data *newd)
{
  static int last_desc = 0;	/* last descriptor number */

  newd->idle_tics = 0;
  newd->output = newd->small_outbuf;
  newd->bufspace = SMALL_BUFSIZE - 1;
  newd->login_time = time(0);
  *newd->output = '\0';
  newd->bufptr = 0;
  newd->has_prompt = 1;  /* prompt is part of greetings */
  STATE(newd) = CONFIG_PROTOCOL_NEGOTIATION ? CON_GET_PROTOCOL : CON_GET_NAME;
  CREATE(newd->history, char *, HISTORY_SIZE);
  if (++last_desc == 1000)
    last_desc = 1;
  newd->desc_num = last_desc;
  newd->pProtocol = ProtocolCreate(); /* KaVir's plugin*/
  newd->events = create_list();
}

/* Send all of the output that we've accumulated for a player out to the
 * player's descriptor. 32 byte GARBAGE_SPACE in MAX_SOCK_BUF used for:
 *	 2 bytes: prepended \r\n
 *	14 bytes: overflow message
 *	 2 bytes: extra \r\n for non-comapct
 *      14 bytes: unused */
static int process_output(struct descriptor_data *t)
{
  char i[MAX_SOCK_BUF], *osb = i + 2;
  int result;

  /* we may need this \r\n for later -- see below */
  strcpy(i, "\r\n");	/* strcpy: OK (for 'MAX_SOCK_BUF >= 3') */

  /* now, append the 'real' output */
  strcpy(osb, t->output);	/* strcpy: OK (t->output:LARGE_BUFSIZE < osb:MAX_SOCK_BUF-2) */

  /* if we're in the overflow state, notify the user */
  if (t->bufspace == 0)
    strcat(osb, "**OVERFLOW**\r\n");	/* strcpy: OK (osb:MAX_SOCK_BUF-2 reserves space) */

  /* add the extra CRLF if the person isn't in compact mode */
  if (STATE(t) == CON_PLAYING && t->character && !IS_NPC(t->character) && !PRF_FLAGGED(t->character, PRF_COMPACT))
    if ( !t->pProtocol->WriteOOB ) 
      strcat(osb, "\r\n");	/* strcpy: OK (osb:MAX_SOCK_BUF-2 reserves space) */

  if (!t->pProtocol->WriteOOB) /* add a prompt */
    strcat(i, make_prompt(t));	/* strcpy: OK (i:MAX_SOCK_BUF reserves space) */

  /* now, send the output.  If this is an 'interruption', use the prepended
   * CRLF, otherwise send the straight output sans CRLF. */
  if (t->has_prompt && !t->pProtocol->WriteOOB) {
    t->has_prompt = FALSE;
    result = write_to_descriptor(t, i);
    if (result >= 2)
      result -= 2;
  } else
    result = write_to_descriptor(t, osb);

  if (result < 0) {	/* Oops, fatal error. Bye! */
//    close_socket(t); // close_socket is called after return of negative result
    return (-1);
  } else if (result == 0)	/* Socket buffer full. Try later. */
    return (0);

  /* Handle snooping: prepend "% " and send to snooper. */
  if (t->snoop_by)
    write_to_output(t->snoop_by, "%% %*s%%%%", result, t->output);

  /* The common case: all saved output was handed off to the kernel buffer. */
  if (result >= t->bufptr) {
    /* If we were using a large buffer, put the large buffer on the buffer pool
     * and switch back to the small one. */
    if (t->large_outbuf) {
      t->large_outbuf->next = bufpool;
      bufpool = t->large_outbuf;
      t->large_outbuf = NULL;
      t->output = t->small_outbuf;
    }
    /* reset total bufspace back to that of a small buffer */
    t->bufspace = SMALL_BUFSIZE - 1;
    t->bufptr = 0;
    *(t->output) = '\0';

    /* If the overflow message or prompt were partially written, try to save
     * them. There will be enough space for them if this is true.  'result'
     * is effectively unsigned here anyway. */
    if ((unsigned int)result < strlen(osb)) {
      size_t savetextlen = strlen(osb + result);

      strcat(t->output, osb + result);
      t->bufptr   -= savetextlen;
      t->bufspace += savetextlen;
    }

  } else {
    /* Not all data in buffer sent.  result < output buffersize. */
    strcpy(t->output, t->output + result);	/* strcpy: OK (overlap) */
    t->bufptr   -= result;
    t->bufspace += result;
  }

  return (result);
}

/* ASSUMPTION: There will be no newlines in the raw input buffer when this
 * function is called.  We must maintain that before returning.
 *
 * Ever wonder why 'tmp' had '+8' on it?  The crusty old code could write
 * MAX_INPUT_LENGTH+1 bytes to 'tmp' if there was a '$' as the final character
 * in the input buffer.  This would also cause 'space_left' to drop to -1,
 * which wasn't very happy in an unsigned variable.  Argh. So to fix the
 * above, 'tmp' lost the '+8' since it doesn't need it and the code has been
 * changed to reserve space by accepting one less character. (Do you really
 * need 256 characters on a line?) -gg 1/21/2000 */
/* Refactored process_input for libuv. 
 * This function no longer reads from the socket.
 * It only parses lines that are already in t->inbuf.
 */
static int process_input(struct descriptor_data *t)
{
  int failed_subst;
  char *ptr, *read_point, *write_point, *nl_pos = NULL;
  char tmp[MAX_INPUT_LENGTH];
  size_t space_left;

  /* search for a newline in the input buffer */
  for (ptr = t->inbuf; *ptr && !nl_pos; ptr++)
    if (ISNEWL(*ptr))
      nl_pos = ptr;

  if (nl_pos == NULL)
    return (0);

  /* okay, at this point we have at least one newline in the string; now we
   * can copy the formatted data to a new array for further processing. */

  read_point = t->inbuf;

  while (nl_pos != NULL) {
    write_point = tmp;
    space_left = MAX_INPUT_LENGTH - 1;

    /* The '> 1' reserves room for a '$ => $$' expansion. */
    for (ptr = read_point; (space_left > 1) && (ptr < nl_pos); ptr++) {
      if (*ptr == '\b' || *ptr == 127) { /* handle backspacing or delete key */
	if (write_point > tmp) {
	  if (*(--write_point) == '$') {
	    write_point--;
	    space_left += 2;
	  } else
	    space_left++;
	}
      } else if (isascii(*ptr) && isprint(*ptr)) {
	if ((*(write_point++) = *ptr) == '$') {		/* copy one character */
	  *(write_point++) = '$';	/* if it's a $, double it */
	  space_left -= 2;
	} else
	  space_left--;
      }
    }

    *write_point = '\0';

    if ((space_left <= 0) && (ptr < nl_pos)) {
      char buffer[MAX_INPUT_LENGTH + 64];

      snprintf(buffer, sizeof(buffer), "Line too long.  Truncated to:\r\n%s\r\n", tmp);
      if (write_to_descriptor(t, buffer) < 0)
	return (-1);
    }
    if (t->snoop_by)
      write_to_output(t->snoop_by, "%% %s\r\n", tmp);
    failed_subst = 0;

    if (*tmp == '!' && !(*(tmp + 1)))	/* Redo last command. */
      strcpy(tmp, t->last_input);	/* strcpy: OK (by mutual MAX_INPUT_LENGTH) */
    else if (*tmp == '!' && *(tmp + 1)) {
      char *commandln = (tmp + 1);
      int starting_pos = t->history_pos,
	  cnt = (t->history_pos == 0 ? HISTORY_SIZE - 1 : t->history_pos - 1);

      skip_spaces(&commandln);
      for (; cnt != starting_pos; cnt--) {
	if (t->history[cnt] && is_abbrev(commandln, t->history[cnt])) {
	  strcpy(tmp, t->history[cnt]);	/* strcpy: OK (by mutual MAX_INPUT_LENGTH) */
	  strcpy(t->last_input, tmp);	/* strcpy: OK (by mutual MAX_INPUT_LENGTH) */
          write_to_output(t, "%s\r\n", tmp);
	  break;
	}
        if (cnt == 0)	/* At top, loop to bottom. */
	  cnt = HISTORY_SIZE;
      }
    } else if (*tmp == '^') {
      if (!(failed_subst = perform_subst(t, t->last_input, tmp)))
	strcpy(t->last_input, tmp);	/* strcpy: OK (by mutual MAX_INPUT_LENGTH) */
    } else {
      strcpy(t->last_input, tmp);	/* strcpy: OK (by mutual MAX_INPUT_LENGTH) */
      if (t->history[t->history_pos])
	free(t->history[t->history_pos]);	/* Clear the old line. */
      t->history[t->history_pos] = strdup(tmp);	/* Save the new. */
      if (++t->history_pos >= HISTORY_SIZE)	/* Wrap to top. */
	t->history_pos = 0;
    }

   /* The '--' command flushes the queue. */
   if ( (*tmp == '-') && (*(tmp+1) == '-') && !(*(tmp+2)) )
   {
     write_to_output(t, "All queued commands cancelled.\r\n");
     flush_queues(t);  /* Flush the command queue */
     failed_subst = 1;  /* Allow the read point to be moved, but don't add to queue */
   }

    if (!failed_subst)
      write_to_q(tmp, &t->input, 0);

    /* find the end of this line */
    while (ISNEWL(*nl_pos))
      nl_pos++;

    /* see if there's another newline in the input buffer */
    read_point = ptr = nl_pos;
    for (nl_pos = NULL; *ptr && !nl_pos; ptr++)
      if (ISNEWL(*ptr))
	nl_pos = ptr;
  }

  /* now move the rest of the buffer up to the beginning for the next pass */
  write_point = t->inbuf;
  while (*read_point)
    *(write_point++) = *(read_point++);
  *write_point = '\0';

  return (1);
}

/* Perform substitution for the '^..^' csh-esque syntax orig is the orig string,
 * i.e. the one being modified.  subst contains the substition string, i.e.
 * "^telm^tell" */
static int perform_subst(struct descriptor_data *t, char *orig, char *subst)
{
  char newsub[MAX_INPUT_LENGTH + 5];

  char *first, *second, *strpos;

  /* First is the position of the beginning of the first string (the one to be
   * replaced. */
  first = subst + 1;

  /* now find the second '^' */
  if (!(second = strchr(first, '^'))) {
    write_to_output(t, "Invalid substitution.\r\n");
    return (1);
  }
  /* terminate "first" at the position of the '^' and make 'second' point
   * to the beginning of the second string */
  *(second++) = '\0';

  /* now, see if the contents of the first string appear in the original */
  if (!(strpos = strstr(orig, first))) {
    write_to_output(t, "Invalid substitution.\r\n");
    return (1);
  }
  /* now, we construct the new string for output. */

  /* first, everything in the original, up to the string to be replaced */
  strncpy(newsub, orig, strpos - orig);	/* strncpy: OK (newsub:MAX_INPUT_LENGTH+5 > orig:MAX_INPUT_LENGTH) */
  newsub[strpos - orig] = '\0';

  /* now, the replacement string */
  strncat(newsub, second, MAX_INPUT_LENGTH - strlen(newsub) - 1);	/* strncpy: OK */

  /* now, if there's anything left in the original after the string to
   * replaced, copy that too. */
  if (((strpos - orig) + strlen(first)) < strlen(orig))
    strncat(newsub, strpos + strlen(first), MAX_INPUT_LENGTH - strlen(newsub) - 1);	/* strncpy: OK */

  /* terminate the string in case of an overflow from strncat */
  newsub[MAX_INPUT_LENGTH - 1] = '\0';
  strcpy(subst, newsub);	/* strcpy: OK (by mutual MAX_INPUT_LENGTH) */

  return (0);
}

void close_socket(struct descriptor_data *d)
{
  struct descriptor_data *temp;

  REMOVE_FROM_LIST(d, descriptor_list, next);
  
  if (!uv_is_closing((uv_handle_t *)&d->handle)) {
    uv_close((uv_handle_t *)&d->handle, on_descriptor_close);
  }

  flush_queues(d);

  /* Forget snooping */
  if (d->snooping)
    d->snooping->snoop_by = NULL;

  if (d->snoop_by) {
    write_to_output(d->snoop_by, "Your victim is no longer among us.\r\n");
    d->snoop_by->snooping = NULL;
  }

  if (d->character) {
    /* If we're switched, this resets the mobile taken. */
    d->character->desc = NULL;

    /* Plug memory leak, from Eric Green. */
    if (!IS_NPC(d->character) && PLR_FLAGGED(d->character, PLR_MAILING) && d->str) {
      if (*(d->str))
        free(*(d->str));
      free(d->str);
      d->str = NULL;
    } else if (d->backstr && !IS_NPC(d->character) && !PLR_FLAGGED(d->character, PLR_WRITING)) {
      free(d->backstr);      /* editing description ... not olc */
      d->backstr = NULL;
    }

    add_llog_entry(d->character, LAST_DISCONNECT);

    if (IS_PLAYING(d) || STATE(d) == CON_DISCONNECT) {
      struct char_data *link_challenged = d->original ? d->original : d->character;

      /* We are guaranteed to have a person. */
      act("$n has lost $s link.", TRUE, link_challenged, 0, 0, TO_ROOM);
      save_char(link_challenged);
      mudlog(NRM, MAX(LVL_IMMORT, GET_INVIS_LEV(link_challenged)), TRUE, "Closing link to: %s.", GET_NAME(link_challenged));
    } else {
      mudlog(CMP, LVL_IMMORT, TRUE, "Losing player: %s.", GET_NAME(d->character) ? GET_NAME(d->character) : "<null>");
      free_char(d->character);
    }
  } else
    mudlog(CMP, LVL_IMMORT, TRUE, "Losing descriptor without char.");

  /* JE 2/22/95 -- part of my unending quest to make switch stable */
  if (d->original && d->original->desc)
    d->original->desc = NULL;

  /* Clear the command history. */
  if (d->history) {
    int cnt;
    for (cnt = 0; cnt < HISTORY_SIZE; cnt++)
      if (d->history[cnt])
	free(d->history[cnt]);
    free(d->history);
  }

  if (d->showstr_head)
    free(d->showstr_head);
  if (d->showstr_count)
    free(d->showstr_vector);
  
  /* KaVir's plugin*/
  ProtocolDestroy( d->pProtocol );
 
  /* Mud Events */
  if (d->events->iSize > 0) {
    struct event * pEvent;

    while ((pEvent = simple_list(d->events)) != NULL)
      event_cancel(pEvent);
  }

  free_list(d->events);

  /*. Kill any OLC stuff .*/
  switch (d->connected) {
    case CON_OEDIT:
    case CON_REDIT:
    case CON_ZEDIT:
    case CON_MEDIT:
    case CON_SEDIT:
    case CON_TEDIT:
    case CON_TRIGEDIT:
    case CON_AEDIT:
    case CON_HEDIT:
    case CON_QEDIT:
    case CON_MSGEDIT:
      cleanup_olc(d, CLEANUP_ALL);
      break;
    default:
      break;
  }
}

static void check_idle_passwords(void)
{
  struct descriptor_data *d, *next_d;

  for (d = descriptor_list; d; d = next_d) {
    next_d = d->next;
    if (STATE(d) != CON_PASSWORD && STATE(d) != CON_GET_NAME)
      continue;
    if (!d->idle_tics) {
      d->idle_tics++;
      continue;
    } else {
      echo_on(d);
      write_to_output(d, "\r\nTimed out... goodbye.\r\n");
      STATE(d) = CON_CLOSE;
    }
  }
}

/*  signal-handling functions (formerly signals.c).  UNIX only. */
#if defined(CIRCLE_UNIX) || defined(CIRCLE_MACINTOSH)
static RETSIGTYPE reread_wizlists(int sig)
{
  reread_wizlist = TRUE;
}

static RETSIGTYPE unrestrict_game(int sig)
{
  emergency_unban = TRUE;
}

#ifdef CIRCLE_UNIX

/* clean up our zombie kids to avoid defunct processes */
static RETSIGTYPE reap(int sig)
{
  while (waitpid(-1, NULL, WNOHANG) > 0);

  my_signal(SIGCHLD, reap);
}

/* Dying anyway... */
static RETSIGTYPE checkpointing(int sig)
{
#ifndef MEMORY_DEBUG
  if (!tics_passed) {
    log("SYSERR: CHECKPOINT shutdown: tics not updated. (Infinite loop suspected)");
    abort();
  } else
    tics_passed = 0;
#endif
}

/* Dying anyway... */
static RETSIGTYPE hupsig(int sig)
{
  log("SYSERR: Received SIGHUP, SIGINT, or SIGTERM.  Shutting down...");
  exit(1); /* perhaps something more elegant should substituted */
}

#endif	/* CIRCLE_UNIX */

/* This is an implementation of signal() using sigaction() for portability.
 * (sigaction() is POSIX; signal() is not.)  Taken from Stevens' _Advanced
 * Programming in the UNIX Environment_.  We are specifying that all system
 * calls _not_ be automatically restarted for uniformity, because BSD systems
 * do not restart select(), even if SA_RESTART is used.
 * Note that NeXT 2.x is not POSIX and does not have sigaction; therefore,
 * I just define it to be the old signal.  If your system doesn't have
 * sigaction either, you can use the same fix.
 * SunOS Release 4.0.2 (sun386) needs this too, according to Tim Aldric. */

#ifndef POSIX
#define my_signal(signo, func) signal(signo, func)
#else
static sigfunc *my_signal(int signo, sigfunc *func)
{
  struct sigaction sact, oact;

  sact.sa_handler = func;
  sigemptyset(&sact.sa_mask);
  sact.sa_flags = 0;
#ifdef SA_INTERRUPT
  sact.sa_flags |= SA_INTERRUPT;	/* SunOS */
#endif

  if (sigaction(signo, &sact, &oact) < 0)
    return (SIG_ERR);

  return (oact.sa_handler);
}
#endif				/* POSIX */

static void signal_setup(void)
{
#ifndef CIRCLE_MACINTOSH
  struct itimerval itime;
  struct timeval interval;

  /* user signal 1: reread wizlists.  Used by autowiz system. */
  my_signal(SIGUSR1, reread_wizlists);

  /* user signal 2: unrestrict game.  Used for emergencies if you lock
   * yourself out of the MUD somehow. */
  my_signal(SIGUSR2, unrestrict_game);

  /* set up the deadlock-protection so that the MUD aborts itself if it gets
   * caught in an infinite loop for more than 3 minutes. */
  interval.tv_sec = 180;
  interval.tv_usec = 0;
  itime.it_interval = interval;
  itime.it_value = interval;
  setitimer(ITIMER_VIRTUAL, &itime, NULL);
  my_signal(SIGVTALRM, checkpointing);

  /* just to be on the safe side: */
  my_signal(SIGHUP, hupsig);
  my_signal(SIGCHLD, reap);
#endif /* CIRCLE_MACINTOSH */
  my_signal(SIGINT, hupsig);
  my_signal(SIGTERM, hupsig);
  my_signal(SIGPIPE, SIG_IGN);
  my_signal(SIGALRM, SIG_IGN);
}

#endif	/* CIRCLE_UNIX || CIRCLE_MACINTOSH */
/* Public routines for system-to-player-communication. */
void game_info(const char *format, ...)
{
  struct descriptor_data *i;
  va_list args;
  char messg[MAX_STRING_LENGTH];
  if (format == NULL)
    return;
  sprintf(messg, "\tcInfo: \ty");
  for (i = descriptor_list; i; i = i->next) {
    if (STATE(i) != CON_PLAYING)
      continue;
    if (!(i->character))
      continue;

    write_to_output(i, "%s", messg);
    va_start(args, format);
    vwrite_to_output(i, format, args);
    va_end(args);
    write_to_output(i, "\tn\r\n");
  }
}

size_t send_to_char(struct char_data *ch, const char *messg, ...)
{
  if (ch->desc && messg && *messg) {
    size_t left;
    va_list args;

    va_start(args, messg);
    left = vwrite_to_output(ch->desc, messg, args);
    va_end(args);
    return left;
  }
  return 0;
}

void send_to_all(const char *messg, ...)
{
  struct descriptor_data *i;
  va_list args;

  if (messg == NULL)
    return;

  for (i = descriptor_list; i; i = i->next) {
    if (STATE(i) != CON_PLAYING)
      continue;

    va_start(args, messg);
    vwrite_to_output(i, messg, args);
    va_end(args);
  }
}

void send_to_outdoor(const char *messg, ...)
{
  struct descriptor_data *i;
  va_list args;

  if (!messg || !*messg)
    return;

  for (i = descriptor_list; i; i = i->next) {

    if (STATE(i) != CON_PLAYING || i->character == NULL)
      continue;
    if (!AWAKE(i->character) || !OUTSIDE(i->character))
      continue;

    va_start(args, messg);
    vwrite_to_output(i, messg, args);
    va_end(args);
  }
}

void send_to_room(room_rnum room, const char *messg, ...)
{
  struct char_data *i;
  va_list args;

  if (messg == NULL)
    return;

  for (i = world[room].people; i; i = i->next_in_room) {
    if (!i->desc)
      continue;

    va_start(args, messg);
    vwrite_to_output(i->desc, messg, args);
    va_end(args);
  }
}

/* Sends a message to the entire group, except for ch.
 * Send 'ch' as NULL, if you want to message to reach
 * everyone. -Vatiken */
void send_to_group(struct char_data *ch, struct group_data *group, const char * msg, ...)
{
	struct char_data *tch;
  va_list args;

  if (msg == NULL)
    return;
    	
  while ((tch = simple_list(group->members)) != NULL) {
    if (tch != ch && !IS_NPC(tch) && tch->desc && STATE(tch->desc) == CON_PLAYING) {
      write_to_output(tch->desc, "%s[%sGroup%s]%s ", 
      CCGRN(tch, C_NRM), CBGRN(tch, C_NRM), CCGRN(tch, C_NRM), CCNRM(tch, C_NRM));
      va_start(args, msg);
      vwrite_to_output(tch->desc, msg, args);
      va_end(args);
    }
  }
}


/* Thx to Jamie Nelson of 4D for this contribution */
void send_to_range(room_vnum start, room_vnum finish, const char *messg, ...)
{
  struct char_data *i;
  va_list args;
  int j;

  if (start > finish) {
    log("send_to_range passed start room value greater then finish.");
    return;
  }
  if (messg == NULL)
    return;

  for (j = 0; j < top_of_world; j++) {
    if (GET_ROOM_VNUM(j) >= start && GET_ROOM_VNUM(j) <= finish) {
      for (i = world[j].people; i; i = i->next_in_room) {
        if (!i->desc)
          continue;

        va_start(args, messg);
        vwrite_to_output(i->desc, messg, args);
        va_end(args);
      }
    }
  }
}

static const char *ACTNULL = "<NULL>";
#define CHECK_NULL(pointer, expression) \
  if ((pointer) == NULL) i = ACTNULL; else i = (expression);
/* higher-level communication: the act() function */
void perform_act(const char *orig, struct char_data *ch, struct obj_data *obj,
    void *vict_obj, struct char_data *to)
{
  const char *i = NULL;
  char lbuf[MAX_STRING_LENGTH], *buf, *j;
  bool uppercasenext = FALSE;
  struct char_data *dg_victim = (to == vict_obj) ? vict_obj : NULL;
  struct obj_data *dg_target = NULL;
  char *dg_arg = NULL;

  buf = lbuf;

  for (;;) {
    if (*orig == '$') {
      switch (*(++orig)) {
      case 'n':
	i = PERS(ch, to);
	break;
      case 'N':
	CHECK_NULL(vict_obj, PERS((const struct char_data *) vict_obj, to));
	dg_victim = (struct char_data *) vict_obj;
	break;
      case 'm':
	i = HMHR(ch);
	break;
      case 'M':
	CHECK_NULL(vict_obj, HMHR((const struct char_data *) vict_obj));
	dg_victim = (struct char_data *) vict_obj;
	break;
      case 's':
	i = HSHR(ch);
	break;
      case 'S':
	CHECK_NULL(vict_obj, HSHR((const struct char_data *) vict_obj));
	dg_victim = (struct char_data *) vict_obj;
	break;
      case 'e':
	i = HSSH(ch);
	break;
      case 'E':
	CHECK_NULL(vict_obj, HSSH((const struct char_data *) vict_obj));
	dg_victim = (struct char_data *) vict_obj;
	break;
      case 'o':
	CHECK_NULL(obj, OBJN(obj, to));
	break;
      case 'O':
	CHECK_NULL(vict_obj, OBJN((const struct obj_data *) vict_obj, to));
	dg_target = (struct obj_data *) vict_obj;
	break;
      case 'p':
	CHECK_NULL(obj, OBJS(obj, to));
	break;
      case 'P':
	CHECK_NULL(vict_obj, OBJS((const struct obj_data *) vict_obj, to));
	dg_target = (struct obj_data *) vict_obj;
	break;
      case 'a':
	CHECK_NULL(obj, SANA(obj));
	break;
      case 'A':
	CHECK_NULL(vict_obj, SANA((const struct obj_data *) vict_obj));
	dg_target = (struct obj_data *) vict_obj;
	break;
       case 'T':
 	CHECK_NULL(vict_obj, (const char *) vict_obj);
 	dg_arg = (char *) vict_obj;
	break;
      case 't':
 	CHECK_NULL(obj, (char *) obj);
	break;
      case 'F':
	CHECK_NULL(vict_obj, fname((const char *) vict_obj));
	break;
      /* uppercase previous word */
      case 'u':
        for (j=buf; j > lbuf && !isspace((int) *(j-1)); j--);
        if (j != buf)
          *j = UPPER(*j);
        i = "";
        break;
      /* uppercase next word */
      case 'U':
        uppercasenext = TRUE;
        i = "";
        break;
      case '$':
	i = "$";
	break;
      default:
	log("SYSERR: Illegal $-code to act(): %c", *orig);
	log("SYSERR: %s", orig);
	i = "";
	break;
      }
      while ((*buf = *(i++)))
        {
        if (uppercasenext && !isspace((int) *buf))
          {
          *buf = UPPER(*buf);
          uppercasenext = FALSE;
          }
	buf++;
        }
      orig++;
    } else if (!(*(buf++) = *(orig++))) {
      break;
    } else if (uppercasenext && !isspace((int) *(buf-1))) {
      *(buf-1) = UPPER(*(buf-1));
      uppercasenext = FALSE;
    }
  }

  *(--buf) = '\r';
  *(++buf) = '\n';
  *(++buf) = '\0';

  if (to->desc)
    write_to_output(to->desc, "%s", CAP(lbuf));

  if ((IS_NPC(to) && dg_act_check) && (to != ch))
    act_mtrigger(to, lbuf, ch, dg_victim, obj, dg_target, dg_arg);

  if (last_act_message)
    free(last_act_message);
  last_act_message = strdup(lbuf);
}

char *act(const char *str, int hide_invisible, struct char_data *ch,
	 struct obj_data *obj, void *vict_obj, int type)
{
  struct char_data *to;
  int to_sleeping;

  if (!str || !*str)
    return NULL;

  /* Warning: the following TO_SLEEP code is a hack. I wanted to be able to tell
   * act to deliver a message regardless of sleep without adding an additional
   * argument.  TO_SLEEP is 128 (a single bit high up).  It's ONLY legal to
   * combine TO_SLEEP with one other TO_x command.  It's not legal to combine
   * TO_x's with each other otherwise. TO_SLEEP only works because its value
   * "happens to be" a single bit; do not change it to something else.  In
   * short, it is a hack. */

  /* check if TO_SLEEP is there, and remove it if it is. */
  if ((to_sleeping = (type & TO_SLEEP)))
    type &= ~TO_SLEEP;

  /* this is a hack as well - DG_NO_TRIG is 256 -- Welcor */
  /* If the bit is set, unset dg_act_check, thus the ! below */
  if (!(dg_act_check = !IS_SET(type, DG_NO_TRIG)))
    REMOVE_BIT(type, DG_NO_TRIG);

  if (type == TO_CHAR) {
    if (ch && SENDOK(ch)) {
      perform_act(str, ch, obj, vict_obj, ch);
      return last_act_message;
    }
    return NULL;
  }

  if (type == TO_VICT) {
    if ((to = vict_obj) != NULL && SENDOK(to)) {
      perform_act(str, ch, obj, vict_obj, to);
      return last_act_message;
    }
    return NULL;
  }

  if (type == TO_GMOTE && !IS_NPC(ch)) {
    struct descriptor_data *i;
    char buf[MAX_STRING_LENGTH];

    for (i = descriptor_list; i; i = i->next) {
      if (!i->connected && i->character &&
          !PRF_FLAGGED(i->character, PRF_NOGOSS) &&
          !PLR_FLAGGED(i->character, PLR_WRITING) &&
          !ROOM_FLAGGED(IN_ROOM(i->character), ROOM_SOUNDPROOF)) {

        sprintf(buf, "%s%s%s", CCYEL(i->character, C_NRM), str, CCNRM(i->character, C_NRM));
        perform_act(buf, ch, obj, vict_obj, i->character);
      }
    }
    return last_act_message;
  }
  /* ASSUMPTION: at this point we know type must be TO_NOTVICT or TO_ROOM */

  if (ch && IN_ROOM(ch) != NOWHERE)
    to = world[IN_ROOM(ch)].people;
  else if (obj && IN_ROOM(obj) != NOWHERE)
    to = world[IN_ROOM(obj)].people;
  else {
    log("SYSERR: no valid target to act()!");
    return NULL;
  }

  for (; to; to = to->next_in_room) {
    if (!SENDOK(to) || (to == ch))
      continue;
    if (hide_invisible && ch && !CAN_SEE(to, ch))
      continue;
    if (type != TO_ROOM && to == vict_obj)
      continue;
    perform_act(str, ch, obj, vict_obj, to);
  }
  return last_act_message;
}

/* Prefer the file over the descriptor. */
static void setup_log(const char *filename, int fd)
{
  FILE *s_fp;

#if defined(__MWERKS__) || defined(__GNUC__)
  s_fp = stderr;
#else
  if ((s_fp = fdopen(STDERR_FILENO, "w")) == NULL) {
    puts("SYSERR: Error opening stderr, trying stdout.");

    if ((s_fp = fdopen(STDOUT_FILENO, "w")) == NULL) {
      puts("SYSERR: Error opening stdout, trying a file.");

      /* If we don't have a file, try a default. */
      if (filename == NULL || *filename == '\0')
        filename = "log/syslog";
    }
  }
#endif

  if (filename == NULL || *filename == '\0') {
    /* No filename, set us up with the descriptor we just opened. */
    logfile = s_fp;
    puts("Using file descriptor for logging.");
    return;
  }

  /* We honor the default filename first. */
  if (open_logfile(filename, s_fp))
    return;

  /* Well, that failed but we want it logged to a file so try a default. */
  if (open_logfile("log/syslog", s_fp))
    return;

  /* Ok, one last shot at a file. */
  if (open_logfile("syslog", s_fp))
    return;

  /* Erp, that didn't work either, just die. */
  puts("SYSERR: Couldn't open anything to log to, giving up.");
  exit(1);
}

static int open_logfile(const char *filename, FILE *stderr_fp)
{
  if (stderr_fp)	/* freopen() the descriptor. */
    logfile = freopen(filename, "w", stderr_fp);
  else
    logfile = fopen(filename, "w");

  if (logfile) {
    printf("Using log file '%s'%s.\n",
		filename, stderr_fp ? " with redirection" : "");
    return (TRUE);
  }

  printf("SYSERR: Error opening file '%s': %s\n", filename, strerror(errno));
  return (FALSE);
}

/* KaVir's plugin*/
static void msdp_update( void )
{
  struct descriptor_data *d;
  int PlayerCount = 0;
  char buf[MAX_STRING_LENGTH];
  extern const char *pc_class_types[];

  for (d = descriptor_list; d; d = d->next)
  {
    struct char_data *ch = d->character;
    if ( ch && !IS_NPC(ch) && d->connected == CON_PLAYING )
    {
      struct char_data *pOpponent = FIGHTING(ch);
      ++PlayerCount;

      MSDPSetString( d, eMSDP_CHARACTER_NAME, GET_NAME(ch) );
      MSDPSetNumber( d, eMSDP_ALIGNMENT, GET_ALIGNMENT(ch) );
      MSDPSetNumber( d, eMSDP_EXPERIENCE, GET_EXP(ch) );

      MSDPSetNumber( d, eMSDP_HEALTH, GET_HIT(ch) );
      MSDPSetNumber( d, eMSDP_HEALTH_MAX, GET_MAX_HIT(ch) );
      MSDPSetNumber( d, eMSDP_LEVEL, GET_LEVEL(ch) );

      sprinttype( ch->player.chclass, pc_class_types, buf, sizeof(buf) );
      MSDPSetString( d, eMSDP_CLASS, buf );

      MSDPSetNumber( d, eMSDP_MANA, GET_MANA(ch) );
      MSDPSetNumber( d, eMSDP_MANA_MAX, GET_MAX_MANA(ch) );
      MSDPSetNumber( d, eMSDP_WIMPY, GET_WIMP_LEV(ch) );
      MSDPSetNumber( d, eMSDP_MONEY, GET_GOLD(ch) );
      MSDPSetNumber( d, eMSDP_MOVEMENT, GET_MOVE(ch) );
      MSDPSetNumber( d, eMSDP_MOVEMENT_MAX, GET_MAX_MOVE(ch) );
      MSDPSetNumber( d, eMSDP_AC, compute_armor_class(ch) );

      /* This would be better moved elsewhere */
      if ( pOpponent != NULL )
      {
          int hit_points = (GET_HIT(pOpponent) * 100) / GET_MAX_HIT(pOpponent);
          MSDPSetNumber( d, eMSDP_OPPONENT_HEALTH, hit_points );
          MSDPSetNumber( d, eMSDP_OPPONENT_HEALTH_MAX, 100 );
          MSDPSetNumber( d, eMSDP_OPPONENT_LEVEL, GET_LEVEL(pOpponent) );
          MSDPSetString( d, eMSDP_OPPONENT_NAME, PERS(pOpponent, ch) );
      }
      else /* Clear the values */
      {
          MSDPSetNumber( d, eMSDP_OPPONENT_HEALTH, 0 );
          MSDPSetNumber( d, eMSDP_OPPONENT_LEVEL, 0 ); 
          MSDPSetString( d, eMSDP_OPPONENT_NAME, "" ); 
      }

      MSDPUpdate( d );
    }

    /* Ideally this should be called once at startup, and again whenever
     * someone leaves or joins the mud.  But this works, and it keeps the
     * snippet simple.  Optimise as you see fit.
     */
    MSSPSetPlayers( PlayerCount );
  }
}
