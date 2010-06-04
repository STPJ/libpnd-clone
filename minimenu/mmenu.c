
/* minimenu
 * aka "2wm" - too weak menu, two week menu, akin to twm
 *
 * Craig wants a super minimal menu ASAP before launch, so lets see what I can put together in 2 weeks with not much
 * free time ;) I'd like to do a fuller ('tiny', but with plugin support and a decent expansion and customizing design..)
 * but later, baby!
 *
 */

/* mmenu - This is the actual menu
 * The goal of this app is to show a application picker screen of some sort, allow the user to perform some useful
 * activities (such as set clock speed, say), and request an app to run, or shutdown, etc.
 * To keep the memory footprint down, when invoking an application, the menu _exits_, and simply spits out
 * an operation for mmwrapper to perform. In the case of no wrapper, the menu will just exit, which is handy for
 * debugging.
 */

/* mmenu lifecycle:
 * 1) determine app list (via pnd scan, .desktop scan, whatever voodoo)
 * 2) show a menu, allow user to interact:
 *    a) user picks an application to run, or -> exit, pass shell run line to wrapper
 *    b) user requests menu shutdown -> exit, tell wrapper to exit as well
 *    c) user performsn some operation (set clock, copy files, whatever) -> probably stays within the menu
 */

#include <stdio.h> /* for FILE etc */
#include <stdlib.h> /* for malloc */
#include <unistd.h> /* for unlink */
#include <limits.h> /* for PATH_MAX */
#include <sys/types.h>
#include <sys/stat.h>
#define __USE_GNU /* for strcasestr */
#include <string.h> /* for making ftw.h happy */
#include <strings.h>
#include <ctype.h>
#include <sys/wait.h>
#include <dirent.h>
#include <signal.h> // for sigaction

#include "pnd_logger.h"
#include "pnd_pxml.h"
#include "pnd_utility.h"
#include "pnd_conf.h"
#include "pnd_container.h"
#include "pnd_discovery.h"
#include "pnd_locate.h"
#include "pnd_device.h"
#include "pnd_pndfiles.h"
#include "../lib/pnd_pathiter.h"

#include "mmenu.h"
#include "mmwrapcmd.h"
#include "mmapps.h"
#include "mmcache.h"
#include "mmcat.h"
#include "mmui.h"

pnd_box_handle g_active_apps = NULL;
unsigned int g_active_appcount = 0;
char g_username [ 128 ]; // since we have to wait for login (!!), store username here
pnd_conf_handle g_conf = 0;
pnd_conf_handle g_desktopconf = 0;

char *pnd_run_script = NULL;
unsigned char g_x11_present = 1; // >0 if X is present
unsigned char g_catmap = 0; // if 1, we're doing category mapping
unsigned char g_pvwcache = 0; // if 1, we're trying to do preview caching

char g_skin_selected [ 100 ] = "default";
char *g_skinpath = NULL; // where 'skin_selected' is located .. the fullpath including skin-dir-name
pnd_conf_handle g_skinconf = NULL;

void sigquit_handler ( int n );

int main ( int argc, char *argv[] ) {
  int logall = -1; // -1 means normal logging rules; >=0 means log all!
  int i;

  // boilerplate stuff from pndnotifyd and pndevmapperd

  /* iterate across args
   */
  for ( i = 1; i < argc; i++ ) {

    if ( argv [ i ][ 0 ] == '-' && argv [ i ][ 1 ] == 'l' ) {

      if ( isdigit ( argv [ i ][ 2 ] ) ) {
	unsigned char x = atoi ( argv [ i ] + 2 );
	if ( x >= 0 &&
	     x < pndn_none )
	{
	  logall = x;
	}
      } else {
	logall = 0;
      }

    } else {
      //printf ( "Unknown: %s\n", argv [ i ] );
      printf ( "%s [-l##]\n", argv [ 0 ] );
      printf ( "-l#\tLog-it; -l is 0-and-up (or all), and -l2 means 2-and-up (not all); l[0-3] for now. Log goes to /tmp/mmenu.log\n" );
      printf ( "-f\tFull path of frontend to run\n" );
      exit ( 0 );
    }

  } // for

  /* enable logging?
   */
  pnd_log_set_pretext ( "mmenu" );
  pnd_log_set_flush ( 1 );

  if ( logall == -1 ) {
    // standard logging; non-daemon versus daemon

#if 1 // HACK: set debug level to high on desktop, but not on pandora; just a convenience while devving, until the conf file is read
    struct stat statbuf;
    if ( stat ( PND_DEVICE_BATTERY_GAUGE_PERC, &statbuf ) == 0 ) {
      // on pandora
      pnd_log_set_filter ( pndn_error );
    } else {
      pnd_log_set_filter ( pndn_debug );
    }
#endif

    pnd_log_to_stdout();

  } else {
    FILE *f;

    f = fopen ( "/tmp/mmenu.log", "w" );

    if ( f ) {
      pnd_log_set_filter ( logall );
      pnd_log_to_stream ( f );
      pnd_log ( pndn_rem, "logall mode - logging to /tmp/mmenu.log\n" );
    }

    if ( logall == pndn_debug ) {
      pnd_log_set_buried_logging ( 1 ); // log the shit out of it
      pnd_log ( pndn_rem, "logall mode 0 - turned on buried logging\n" );
    }

  } // logall

  pnd_log ( pndn_rem, "%s built %s %s", argv [ 0 ], __DATE__, __TIME__ );

  pnd_log ( pndn_rem, "log level starting as %u", pnd_log_get_filter() );

  // wait for a user to be logged in - we should probably get hupped when a user logs in, so we can handle
  // log-out and back in again, with SDs popping in and out between..
  pnd_log ( pndn_rem, "Checking to see if a user is logged in\n" );
  while ( 1 ) {
    if ( pnd_check_login ( g_username, 127 ) ) {
      break;
    }
    pnd_log ( pndn_debug, "  No one logged in yet .. spinning.\n" );
    sleep ( 2 );
  } // spin
  pnd_log ( pndn_rem, "Looks like user '%s' is in, continue.\n", g_username );

  /* conf file
   */
  g_conf = pnd_conf_fetch_by_name ( MMENU_CONF, MMENU_CONF_SEARCHPATH );

  if ( ! g_conf ) {
    pnd_log ( pndn_error, "ERROR: Couldn't fetch conf file '%s'!\n", MMENU_CONF );
    emit_and_quit ( MM_QUIT );
  }

  g_desktopconf = pnd_conf_fetch_by_id ( pnd_conf_desktop, PND_CONF_SEARCHPATH );

  if ( ! g_desktopconf ) {
    pnd_log ( pndn_error, "ERROR: Couldn't fetch desktop conf file\n" );
    emit_and_quit ( MM_QUIT );
  }

  /* set up quit signal handler
   */
  sigset_t ss;
  sigemptyset ( &ss );

  struct sigaction siggy;
  siggy.sa_handler = sigquit_handler;
  siggy.sa_mask = ss; /* implicitly blocks the origin signal */
  siggy.sa_flags = SA_RESTART; /* don't need anything */
  sigaction ( SIGQUIT, &siggy, NULL );

  /* category conf file
   */
  {
    char *locater = pnd_locate_filename ( pnd_conf_get_as_char ( g_conf, "categories.catmap_searchpath" ),
					  pnd_conf_get_as_char ( g_conf, "categories.catmap_confname" ) );

    if ( locater ) {
      pnd_log ( pndn_rem, "Found category conf at '%s'\n", locater );
      pnd_conf_handle h = pnd_conf_fetch_by_path ( locater );
      if ( h ) {
	// lets just merge the skin conf onto the regular conf, so it just magicly works
	pnd_box_append ( g_conf, h );
      }
    } else {
      pnd_log ( pndn_debug, "No additional category conf file found.\n" );
    }

  } // cat conf

  // redo log filter
  pnd_log_set_filter ( pnd_conf_get_as_int_d ( g_conf, "minimenu.loglevel", pndn_error ) );

  /* setup
   */

  // X11?
  if ( pnd_conf_get_as_char ( g_conf, "minimenu.x11_present_sh" ) ) {
    FILE *fx = popen ( pnd_conf_get_as_char ( g_conf, "minimenu.x11_present_sh" ), "r" );
    char buffer [ 100 ];
    if ( fx ) {
      if ( fgets ( buffer, 100, fx ) ) {
	if ( atoi ( buffer ) ) {
	  g_x11_present = 1;
	  pnd_log ( pndn_rem, "X11 seems to be present [pid %u]\n", atoi(buffer) );
	} else {
	  g_x11_present = 0;
	  pnd_log ( pndn_rem, "X11 seems NOT to be present\n" );
	}
      } else {
	  g_x11_present = 0;
	  pnd_log ( pndn_rem, "X11 seems NOT to be present\n" );
      }
      pclose ( fx );
    }
  } // x11?

  // pnd_run.sh
  pnd_run_script = pnd_locate_filename ( pnd_conf_get_as_char ( g_conf, "minimenu.pndrun" ), "pnd_run.sh" );

  if ( ! pnd_run_script ) {
    pnd_log ( pndn_error, "ERROR: Couldn't locate pnd_run.sh!\n" );
    emit_and_quit ( MM_QUIT );
  }

  pnd_run_script = strdup ( pnd_run_script ); // so we don't lose it next pnd_locate

  pnd_log ( pndn_rem, "Found pnd_run.sh at '%s'\n", pnd_run_script );

  // figure out what skin is selected (or default)
  FILE *f;
  char *s = strdup ( pnd_conf_get_as_char ( g_conf, "minimenu.skin_selected" ) );
  s = pnd_expand_tilde ( s );
  if ( ( f = fopen ( s, "r" ) ) ) {
    char buffer [ 100 ];
    if ( fgets ( buffer, 100, f ) ) {
      // see if that dir can be located
      if ( strchr ( buffer, '\n' ) ) {
	* strchr ( buffer, '\n' ) = '\0';
      }
      char *found = pnd_locate_filename ( pnd_conf_get_as_char ( g_conf, "minimenu.skin_searchpath" ), buffer );
      if ( found ) {
	strncpy ( g_skin_selected, buffer, 100 );
	g_skinpath = strdup ( found );
      } else {
	pnd_log ( pndn_warning, "Couldn't locate skin named '%s' so falling back.\n", buffer );
      }
    }
    fclose ( f );
  }
  free ( s );

  if ( g_skinpath ) {
    pnd_log ( pndn_rem, "Skin is selected: '%s'\n", g_skin_selected );
  } else {
    pnd_log ( pndn_rem, "Skin falling back to: '%s'\n", g_skin_selected );

    char *found = pnd_locate_filename ( pnd_conf_get_as_char ( g_conf, "minimenu.skin_searchpath" ),
					g_skin_selected );
    if ( found ) {
      g_skinpath = strdup ( found );
    } else {
      pnd_log ( pndn_error, "Couldn't locate skin named '%s'.\n", g_skin_selected );
      emit_and_quit ( MM_QUIT );
    }

  }
  pnd_log ( pndn_rem, "Skin path determined to be: '%s'\n", g_skinpath );

  // lets see if this skin-path actually has a skin conf
  {
    char fullpath [ PATH_MAX ];
    sprintf ( fullpath, "%s/%s", g_skinpath, pnd_conf_get_as_char ( g_conf, "minimenu.skin_confname" ) );
    g_skinconf = pnd_conf_fetch_by_path ( fullpath );
  }

  // figure out skin path if we didn't get it already
  if ( ! g_skinconf ) {
    pnd_log ( pndn_error, "ERROR: Couldn't set up skin!\n" );
    emit_and_quit ( MM_QUIT );
  }

  // lets just merge the skin conf onto the regular conf, so it just magicly works
  pnd_box_append ( g_conf, g_skinconf );

  // attempt to set up UI
  if ( ! ui_setup() ) {
    pnd_log ( pndn_error, "ERROR: Couldn't set up the UI!\n" );
    emit_and_quit ( MM_QUIT );
  }

  // show load screen
  ui_loadscreen();

  // store flag if we're doing preview caching or not
  if ( pnd_conf_get_as_int_d ( g_conf, "previewpic.do_cache", 0 ) ) {
    g_pvwcache = 1;
  }

  // set up static image cache
  if ( ! ui_imagecache ( g_skinpath ) ) {
    pnd_log ( pndn_error, "ERROR: Couldn't set up static UI image cache!\n" );
    emit_and_quit ( MM_QUIT );
  }

  // create all cat
  if ( pnd_conf_get_as_int_d ( g_conf, "categories.do_all_cat", 1 ) ) {
    category_push ( g_x11_present ? CATEGORY_ALL "    (X11)" : CATEGORY_ALL "   (No X11)", NULL /*app*/, 0, NULL /* fspath */ );
  }

  // set up category mappings
  if ( pnd_conf_get_as_int_d ( g_conf, "categories.map_on", 0 ) ) {
    g_catmap = category_map_setup();
  }

  /* inhale applications, icons, categories, etc
   */
  applications_scan();

  /* actual work now
   */

  while ( 1 ) { // forever!

    // show the menu, or changes thereof
    ui_render();

    // wait for input or time-based events (like animations)
    // deal with inputs
    ui_process_input ( 1 /* block */ );

    // sleep? block?
    usleep ( 5000 );

  } // while

  return ( 0 );
}

void emit_and_quit ( char *s ) {
  printf ( "%s\n", s );
  exit ( 0 );
}

static unsigned int is_dir_empty ( char *fullpath ) {
  DIR *d = opendir ( fullpath );

  if ( ! d ) {
    return ( 0 ); // not empty, since we don't know
  }

  struct dirent *de = readdir ( d );

  while ( de ) {

    if ( strcmp ( de -> d_name, "." ) == 0 ) {
      // irrelevent
    } else if ( strcmp ( de -> d_name, ".." ) == 0 ) {
      // irrelevent
    } else {
      // something else came in, so dir must not be empty
      closedir ( d );
      return ( 0 ); 
    }

    de = readdir ( d );
  }

  closedir ( d );

  return ( 1 ); // dir is empty
}

void applications_free ( void ) {

  // free up all our category apprefs, but keep the preview and icon cache's..
  category_freeall();

  // free up old disco_t
  if ( g_active_apps ) {
    pnd_disco_t *p = pnd_box_get_head ( g_active_apps );
    pnd_disco_t *n;
    while ( p ) {
      n = pnd_box_get_next ( p );
      pnd_disco_destroy ( p );
      p = n;
    }
    pnd_box_delete ( g_active_apps );
  }

  return;
}

void applications_scan ( void ) {

  // show disco screen
  ui_discoverscreen ( 1 /* clear screen */ );

  // determine current app list, cache icons
  // - ignore overrides for now

  g_active_apps = 0;
  pnd_box_handle merge_apps = 0;

  // desktop apps?
  if ( pnd_conf_get_as_int_d ( g_conf, "minimenu.desktop_apps", 1 ) ) {
    pnd_log ( pndn_debug, "Looking for pnd applications here: %s\n",
	      pnd_conf_get_as_char ( g_desktopconf, "desktop.searchpath" ) );
    g_active_apps = pnd_disco_search ( pnd_conf_get_as_char ( g_desktopconf, "desktop.searchpath" ), NULL );
  }

  // menu apps?
  if ( pnd_conf_get_as_int_d ( g_conf, "minimenu.menu_apps", 1 ) ) {
    pnd_log ( pndn_debug, "Looking for pnd applications here: %s\n",
	      pnd_conf_get_as_char ( g_desktopconf, "menu.searchpath" ) );
    merge_apps = pnd_disco_search ( pnd_conf_get_as_char ( g_desktopconf, "menu.searchpath" ), NULL );
  }

  // merge lists
  if ( merge_apps ) {
    if ( g_active_apps ) {
      // got menu apps, and got desktop apps, merge
      pnd_box_append ( g_active_apps, merge_apps );
    } else {
      // got menu apps, had no desktop apps, so just assign
      g_active_apps = merge_apps;
    }
  }

  // aux apps?
  char *aux_apps = NULL;
  merge_apps = 0;
  aux_apps = pnd_conf_get_as_char ( g_conf, "minimenu.aux_searchpath" );
  if ( aux_apps && aux_apps [ 0 ] ) {
    pnd_log ( pndn_debug, "Looking for pnd applications here: %s\n", aux_apps );
    merge_apps = pnd_disco_search ( aux_apps, NULL );
  }

  // merge aux apps
  if ( merge_apps ) {
    if ( g_active_apps ) {
      pnd_box_append ( g_active_apps, merge_apps );
    } else {
      g_active_apps = merge_apps;
    }
  }

  // do it
  g_active_appcount = pnd_box_get_size ( g_active_apps );

  unsigned char maxwidth, maxheight;
  maxwidth = pnd_conf_get_as_int_d ( g_conf, "grid.icon_max_width", 50 );
  maxheight = pnd_conf_get_as_int_d ( g_conf, "grid.icon_max_height", 50 );

  // show cache screen
  ui_cachescreen ( 1 /* clear screen */, NULL );

  pnd_log ( pndn_debug, "Found pnd applications, and caching icons:\n" );
  pnd_disco_t *iter = pnd_box_get_head ( g_active_apps );
  unsigned int itercount = 0;
  while ( iter ) {
    //pnd_log ( pndn_debug, "  App: '%s'\n", IFNULL(iter->title_en,"No Name") );

    // update cachescreen
    // ... every 5 filenames, just to avoid slowing it too much
    if ( itercount % 5 == 0 ) {
      ui_cachescreen ( 0 /* clear screen */, IFNULL(iter->title_en,"No Name") );
    }

    // if an ovr was flagged by libpnd, lets go inhale it just so we've got the
    // notes handy, since notes are not handled by libpnd proper
    pnd_conf_handle ovrh = 0;
    if ( iter -> object_flags & PND_DISCO_FLAG_OVR ) {
      char ovrfile [ PATH_MAX ];
      char *fixpxml;
      sprintf ( ovrfile, "%s/%s", iter -> object_path, iter -> object_filename );
      fixpxml = strcasestr ( ovrfile, PND_PACKAGE_FILEEXT );
      strcpy ( fixpxml, PXML_SAMEPATH_OVERRIDE_FILEEXT );

      ovrh = pnd_conf_fetch_by_path ( ovrfile );

#if 0
      if ( ovrh ) {
	pnd_log ( pndn_debug, "Found ovr file for %s # %u\n", iter -> object_filename, iter -> subapp_number );
      }
#endif

    } // ovr

    // cache the icon, unless deferred
    if ( pnd_conf_get_as_int_d ( g_conf, "minimenu.load_icons_later", 0 ) == 0 ) {
      if ( iter -> pnd_icon_pos &&
	   ! cache_icon ( iter, maxwidth, maxheight ) )
      {
	pnd_log ( pndn_warning, "  Couldn't load icon: '%s'\n", IFNULL(iter->title_en,"No Name") );
      }
    }

    // cache the preview --> SHOULD DEFER
    if ( pnd_conf_get_as_int_d ( g_conf, "minimenu.load_previews_now", 0 ) > 0 ) {
      // load the preview pics now!
      if ( iter -> preview_pic1 &&
	   ! cache_preview ( iter, pnd_conf_get_as_int_d ( g_conf, "previewpic.cell_width", 200 ), pnd_conf_get_as_int_d ( g_conf, "previewpic.cell_height", 180 ) ) )
      {
	pnd_log ( pndn_warning, "  Couldn't load preview pic: '%s' -> '%s'\n", IFNULL(iter->title_en,"No Name"), iter -> preview_pic1 );
      }
    } // preview now?

    // push the categories .. or suppress application
    //
    if ( ( pnd_pxml_get_x11 ( iter -> option_no_x11 ) != pnd_pxml_x11_required ) ||
	 ( pnd_pxml_get_x11 ( iter -> option_no_x11 ) == pnd_pxml_x11_required && g_x11_present == 1 )
       )
    {

      // push to All category
      // we do this first, so first category is always All
      if ( pnd_conf_get_as_int_d ( g_conf, "categories.do_all_cat", 1 ) ) {
	category_push ( g_x11_present ? CATEGORY_ALL "    (X11)" : CATEGORY_ALL "   (No X11)", iter, ovrh, NULL /* fspath */ );
      } // all?

      // main categories
      category_meta_push ( iter -> main_category, NULL /* no parent cat */, iter, ovrh, pnd_conf_get_as_int_d ( g_conf, "tabs.top_maincat", 1 ) );
      category_meta_push ( iter -> main_category1, iter -> main_category, iter, ovrh, pnd_conf_get_as_int_d ( g_conf, "tabs.top_maincat1", 0 ) );
      category_meta_push ( iter -> main_category2, iter -> main_category, iter, ovrh, pnd_conf_get_as_int_d ( g_conf, "tabs.top_maincat2", 0 ) );
      // alt categories
      category_meta_push ( iter -> alt_category, NULL /* no parent cat */, iter, ovrh, pnd_conf_get_as_int_d ( g_conf, "tabs.top_altcat", 0 ) );
      category_meta_push ( iter -> alt_category1, iter -> alt_category, iter, ovrh, pnd_conf_get_as_int_d ( g_conf, "tabs.top_altcat1", 0 ) );
      category_meta_push ( iter -> alt_category2, iter -> alt_category, iter, ovrh, pnd_conf_get_as_int_d ( g_conf, "tabs.top_altcat2", 0 ) );

    } // register with categories or filter out

    // next
    iter = pnd_box_get_next ( iter );
    itercount++;
  } // while

  // sort (some) categories
  category_sort();

  // set up filesystem browser tabs
  if ( pnd_conf_get_as_int_d ( g_conf, "filesystem.do_browser", 0 ) ) {
    char *searchpath = pnd_conf_get_as_char ( g_conf, "filesystem.tab_searchpaths" );

    SEARCHPATH_PRE
    {
      char *c, *tabname;
      c = strrchr ( buffer, '/' );
      if ( c && (*(c+1)!='\0') ) {
	tabname = c;
      } else {
	tabname = buffer;
      }

      // check if dir is empty; if so, skip it.
      if ( ! is_dir_empty ( buffer ) ) {
	category_push ( tabname /* tab name */, NULL /* app */, 0 /* override */, buffer /* fspath */ );
      }

    }
    SEARCHPATH_POST

  } // set up fs browser tabs

  // dump categories
  //category_dump();

  // let deferred icon cache go now
  ui_post_scan();

  return;
}

void sigquit_handler ( int n ) {
  pnd_log ( pndn_rem, "SIGQUIT received; graceful exit.\n" );
  emit_and_quit ( MM_QUIT );
}
