/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
  Gpredict: Real-time satellite tracking and orbit prediction program

  Copyright (C)  2001-2010  Alexandru Csete, OZ9AEC.

  Authors: Alexandru Csete <oz9aec@gmail.com>

  Comments, questions and bugreports should be submitted via
  http://sourceforge.net/projects/gpredict/
  More details can be found at the project home page:

  http://gpredict.oz9aec.net/
 
  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.
  
  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.
  
  You should have received a copy of the GNU General Public License
  along with this program; if not, visit http://www.fsf.org/
*/
/** \brief Main module container.
 *
 * The GtkSatModule widget is the top level container that contains the
 * individual views. These views are of type GtkSatList, GtkSatMap, GtkSingleSat,
 * GtkPolarView and GtkEventList (as of version 1.2).
 *
 * The views are organized in a grid (GtkTable) where each view can occupy one
 * or more squares, see the create_module_layout() function.
 * 
 * A module can have a GtkRigCtrl, a GtkRotCtrl and a GtkSkyGlance widget
 * associated to it. These associations exist because theu share QTH and
 * satellite data.
 */
#include <gtk/gtk.h>
#include <glib/gi18n.h>
#include <sys/time.h>
#include "sgpsdp/sgp4sdp4.h"
#include "sat-log.h"
#include "gpredict-utils.h"
#include "config-keys.h"
#include "sat-cfg.h"
#include "mod-cfg.h"
#include "mod-cfg-get-param.h"
#include "mod-mgr.h"
#ifdef HAVE_CONFIG_H
#  include <build-config.h>
#endif
#include "time-tools.h"
#include "orbit-tools.h"
#include "predict-tools.h"
#include "gtk-sat-module.h"
#include "gtk-sat-module-popup.h"
#include "gtk-sat-module-tmg.h"
#include "gtk-sat-list.h"
#include "gtk-sat-map.h"
#include "gtk-polar-view.h"
#include "gtk-single-sat.h"
#include "gtk-event-list.h"
#include "gtk-rig-ctrl.h"
#include "gtk-rot-ctrl.h"
#include "gtk-sky-glance.h"
#include "compat.h"

#include "lo/lo.h"
#include "string.h"


//#ifdef G_OS_WIN32
//#  include "libc_internal.h"
//#  include "libc_interface.h"
//#endif

static void     gtk_sat_module_class_init     (GtkSatModuleClass   *class);
static void     gtk_sat_module_init           (GtkSatModule        *module);
static void     gtk_sat_module_destroy        (GtkObject           *object);
static void     gtk_sat_module_read_cfg_data  (GtkSatModule *module,
                                               const gchar *cfgfile);

static void     gtk_sat_module_load_sats      (GtkSatModule *module);
static void     gtk_sat_module_free_sat       (gpointer sat);
static gboolean gtk_sat_module_timeout_cb     (gpointer module);
static void     gtk_sat_module_update_sat     (gpointer key,
                                               gpointer val,
                                               gpointer data);
static void     gtk_sat_module_popup_cb       (GtkWidget *button,
                                               gpointer data);

static void     update_header                 (GtkSatModule *module);
static void     update_child                  (GtkWidget *child, gdouble tstamp);
static void     create_module_layout          (GtkSatModule *module);
static void     get_grid_size                 (GtkSatModule *module, guint *rows, guint *cols);
static GtkWidget *create_view                 (GtkSatModule *module, guint num);

static void     reload_sats_in_child (GtkWidget *widget, GtkSatModule *module);

static void     update_skg                    (GtkSatModule *module);


static GtkVBoxClass *parent_class = NULL;


GType
gtk_sat_module_get_type ()
{
    static GType gtk_sat_module_type = 0;

    if (!gtk_sat_module_type) {
        static const GTypeInfo gtk_sat_module_info = {
            sizeof (GtkSatModuleClass),
            NULL,  /* base_init */
            NULL,  /* base_finalize */
            (GClassInitFunc) gtk_sat_module_class_init,
            NULL,  /* class_finalize */
            NULL,  /* class_data */
            sizeof (GtkSatModule),
            5,     /* n_preallocs */
            (GInstanceInitFunc) gtk_sat_module_init,
        };

        gtk_sat_module_type = g_type_register_static (GTK_TYPE_VBOX,
                                                      "GtkSatModule",
                                                      &gtk_sat_module_info,
                                                      0);
    }

    return gtk_sat_module_type;
}


static void
gtk_sat_module_class_init (GtkSatModuleClass *class)
{
    GObjectClass      *gobject_class;
    GtkObjectClass    *object_class;
    GtkWidgetClass    *widget_class;
    GtkContainerClass *container_class;

    gobject_class   = G_OBJECT_CLASS (class);
    object_class    = (GtkObjectClass*) class;
    widget_class    = (GtkWidgetClass*) class;
    container_class = (GtkContainerClass*) class;

    parent_class = g_type_class_peek_parent (class);

    object_class->destroy = gtk_sat_module_destroy;

}


/** \brief Initialise GtkSatModule widget */
static void
gtk_sat_module_init (GtkSatModule *module)
{

    /* initialise data structures */
    module->win = NULL;

    module->qth = g_try_new0 (qth_t, 1);
    module->qth->lat = 0.0;
    module->qth->lon = 0.0;
    module->qth->alt = 0;

    module->satellites = g_hash_table_new_full (g_int_hash,
                                                g_int_equal,
                                                g_free,
                                                gtk_sat_module_free_sat);
    
    module->rotctrlwin = NULL;
    module->rotctrl    = NULL;
    module->rigctrlwin = NULL;
    module->rigctrl    = NULL;
    module->skgwin     = NULL;
    module->skg        = NULL;
    module->lastSkgUpd = 0.0;

    module->state = GTK_SAT_MOD_STATE_DOCKED;
    module->busy = g_mutex_new();

    module->grid = NULL;
    module->views = NULL;
    module->nviews = 0;

    module->timerid = 0;
    
    module->throttle = 1;
    module->rtNow = 0.0;
    module->rtPrev = 0.0;
    module->tmgActive = FALSE;
    module->tmgPdnum = 0.0;
    module->tmgCdnum = 0.0;
    module->tmgReset = FALSE;
    
}


static void
gtk_sat_module_destroy (GtkObject *object)
{
    GtkSatModule *module = GTK_SAT_MODULE (object);

    
    /* stop timeout */
    if (module->timerid > 0)
        g_source_remove (module->timerid);

    /* destroy time controller */
    if (module->tmgActive) {
        gtk_widget_destroy (module->tmgWin);
        module->tmgActive = FALSE;
    }
    
    /* destroy radio and rotator controllers */
    if (module->rigctrlwin) {
        gtk_widget_destroy (module->rigctrlwin);
    }
    if (module->rotctrlwin) {
        gtk_widget_destroy (module->rotctrlwin);
    }
    
    /* destroy sky at a glance window */
    if (module->skgwin) {
        gtk_widget_destroy (module->skgwin);
    }

    /* clean up QTH */
    if (module->qth) {
        qth_data_free (module->qth);
        module->qth = NULL;
    }

    /* clean up satellites */
    if (module->satellites) {
        g_hash_table_destroy (module->satellites);
        module->satellites = NULL;
    }

    if (module->grid) {
        g_free (module->grid);
        module->grid = NULL;
    }

    /* FIXME: free module->views? */

    (* GTK_OBJECT_CLASS (parent_class)->destroy) (object);
}


/** \brief Create a new GtkSatModule widget.
 *  \param cfgfile The name of the configuration file (.mod)
 * 
 *  \bug Program goes into infinite loop when there is something
 *       wrong with cfg file.
 */
GtkWidget *
gtk_sat_module_new (const gchar *cfgfile)
{
    GtkWidget *widget;
    GtkWidget *butbox;


    /* Read configuration data.
       If cfgfile is not existing or is NULL, start the wizard
       in order to create a new configuration.
    */
    if ((cfgfile == NULL) || !g_file_test (cfgfile, G_FILE_TEST_EXISTS)) {

        sat_log_log (SAT_LOG_LEVEL_BUG,
                     _("%s: Module %s is not valid."),
                     __FUNCTION__, cfgfile);

        return NULL;
    }

    /* create module widget */
    widget = g_object_new (GTK_TYPE_SAT_MODULE, NULL);

//    g_signal_connect (widget, "realize",
//                      G_CALLBACK (fix_child_allocations), NULL);

    /* load configuration; note that this will also set the module name */
    gtk_sat_module_read_cfg_data (GTK_SAT_MODULE (widget), cfgfile);

     /*check that we loaded some reasonable data*/
     if (GTK_SAT_MODULE(widget)->cfgdata==NULL){
          sat_log_log (SAT_LOG_LEVEL_ERROR,
                     _("%s: Module %s has problems."),
                     __FUNCTION__, cfgfile);
          
        return NULL;
     }
     
    /* module state */
    if ((g_key_file_has_key (GTK_SAT_MODULE (widget)->cfgdata,
                             MOD_CFG_GLOBAL_SECTION,
                             MOD_CFG_STATE, NULL)) &&
        sat_cfg_get_bool (SAT_CFG_BOOL_MOD_STATE)) {

        GTK_SAT_MODULE (widget)->state =
            g_key_file_get_integer (GTK_SAT_MODULE (widget)->cfgdata,
                                    MOD_CFG_GLOBAL_SECTION,
                                    MOD_CFG_STATE, NULL);
    }
    else {
        GTK_SAT_MODULE (widget)->state = GTK_SAT_MOD_STATE_DOCKED;
    }

    /* initialise time keeping vars to current time */
    GTK_SAT_MODULE (widget)->rtNow = get_current_daynum ();
    GTK_SAT_MODULE (widget)->rtPrev = get_current_daynum ();
    GTK_SAT_MODULE (widget)->tmgPdnum = get_current_daynum ();
    GTK_SAT_MODULE (widget)->tmgCdnum = get_current_daynum ();


    /* load satellites */
    gtk_sat_module_load_sats (GTK_SAT_MODULE (widget));
    
    /* create buttons */
    GTK_SAT_MODULE (widget)->popup_button =
        gpredict_mini_mod_button ("gpredict-mod-popup.png",
                                  _("Module options / shortcuts"));
    g_signal_connect (GTK_SAT_MODULE (widget)->popup_button, "clicked",
                      G_CALLBACK (gtk_sat_module_popup_cb), widget);

    GTK_SAT_MODULE (widget)->close_button =
        gpredict_mini_mod_button ("gpredict-mod-close.png",
                                  _("Close this module."));
    g_signal_connect (GTK_SAT_MODULE (widget)->close_button, "clicked",
                      G_CALLBACK (gtk_sat_module_close_cb), widget);

    /* create header; header should not be updated more than
       once pr. second.
    */
    GTK_SAT_MODULE (widget)->header = gtk_label_new (NULL);
    GTK_SAT_MODULE (widget)->head_count = 0;
    GTK_SAT_MODULE (widget)->head_timeout = 
        (GTK_SAT_MODULE(widget)->timeout > 1000 ? 1 : 
         (guint) floor (1000/GTK_SAT_MODULE(widget)->timeout));

    /* Event timeout
       Update every minute FIXME: user configurable
    */
    GTK_SAT_MODULE (widget)->event_timeout = 
        (GTK_SAT_MODULE(widget)->timeout > 60000 ? 1 : 
         (guint) floor (60000/GTK_SAT_MODULE(widget)->timeout));
    /* force update the first time */
    GTK_SAT_MODULE (widget)->event_count = GTK_SAT_MODULE (widget)->event_timeout;


    butbox = gtk_hbox_new (FALSE, 0);
    gtk_box_pack_start (GTK_BOX (butbox),
                        GTK_SAT_MODULE (widget)->header,
                        FALSE, FALSE, 10);
    gtk_box_pack_end (GTK_BOX (butbox),
                      GTK_SAT_MODULE (widget)->close_button,
                      FALSE, FALSE, 0);
    gtk_box_pack_end (GTK_BOX (butbox),
                      GTK_SAT_MODULE (widget)->popup_button,
                      FALSE, FALSE, 0);

    gtk_box_pack_start (GTK_BOX (widget), butbox, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (widget), gtk_hseparator_new (), FALSE, FALSE, 0);

    create_module_layout (GTK_SAT_MODULE (widget));


    gtk_widget_show_all (widget);

    /* start timeout */
    GTK_SAT_MODULE(widget)->timerid = g_timeout_add (GTK_SAT_MODULE(widget)->timeout,
                                                     gtk_sat_module_timeout_cb,
                                                     widget);
    

    return widget;
}


/** \brief Create module layout and add views.
 *
 * It is assumed that module->grid and module->nviews have
 * coherent values.
 */
static void
create_module_layout (GtkSatModule *module)
{
    GtkWidget *table;
    GtkWidget *view;
    guint rows,cols;
    guint i;


    /* calculate the number of rows and columns necessary */
    get_grid_size (module, &rows, &cols);
    sat_log_log (SAT_LOG_LEVEL_DEBUG,
                 _("%s: Layout has %d columns and %d rows."),
                 __FUNCTION__, cols, rows);

    table = gtk_table_new (rows, cols, TRUE);

    for (i = 0; i < module->nviews; i++) {
        /* create the view */
        view = create_view (module, module->grid[5*i]);

        /* store a pointer to the view */
        module->views = g_slist_append (module->views, view);

        /* add view to the grid */
        gtk_table_attach_defaults (GTK_TABLE (table), view,
                                   module->grid[5*i+1], module->grid[5*i+2],
                                   module->grid[5*i+3], module->grid[5*i+4]);
    }

    gtk_container_add (GTK_CONTAINER (module), table);

}


/** \brief Create a new view.
  * \param module Pointer to the parent GtkSatModule widget
  * \param num The number ID of the view to create, see gtk_sat_mod_view_t
  * \return Pointer to a new GtkWidget of type corresponding to num. If num
  *         is invalid, a GtkSatList is returned.
  */
static GtkWidget *
create_view (GtkSatModule *module, guint num)
{
    GtkWidget *view;


    switch (num) {

    case GTK_SAT_MOD_VIEW_LIST:
        view = gtk_sat_list_new (module->cfgdata,
                                 module->satellites,
                                 module->qth,
                                 0);
        break;

    case GTK_SAT_MOD_VIEW_MAP:
        view = gtk_sat_map_new (module->cfgdata,
                                module->satellites,
                                module->qth);
        break;

    case GTK_SAT_MOD_VIEW_POLAR:
        view = gtk_polar_view_new (module->cfgdata,
                                   module->satellites,
                                   module->qth);
        break;

    case GTK_SAT_MOD_VIEW_SINGLE:
        view = gtk_single_sat_new (module->cfgdata,
                                   module->satellites,
                                   module->qth,
                                   0);
        break;

    case GTK_SAT_MOD_VIEW_EVENT:
        view = gtk_event_list_new (module->cfgdata,
                                   module->satellites,
                                   module->qth,
                                   0);
        break;

    default:
        sat_log_log (SAT_LOG_LEVEL_BUG,
                     _("%s:%d: Invalid child type (%d). Using GtkSatList."),
                     __FILE__, __LINE__, num);

        view = gtk_sat_list_new (module->cfgdata,
                                 module->satellites,
                                 module->qth,
                                 0);
        break;

    }


    return view;
}


/** \brief Read moule configuration data.
 *  \ingroup satmodpriv
 *  \param module The GtkSatModule to which the configuration will be applied.
 *  \param cfgfile The configuration file.
 */
static void
gtk_sat_module_read_cfg_data (GtkSatModule *module, const gchar *cfgfile)
{
    gchar   *buffer = NULL;
    gchar   *qthfile;
    gchar   *confdir;
    gchar  **buffv;
    guint   length,i;
    GError  *error = NULL;

    module->cfgdata = g_key_file_new ();
    g_key_file_set_list_separator (module->cfgdata, ';');

    /* Bail out with error message if data can not be read */
    if (!g_key_file_load_from_file (module->cfgdata, cfgfile,
                                    G_KEY_FILE_KEEP_COMMENTS, &error)) {

        g_key_file_free (module->cfgdata);
          module->cfgdata=NULL;
        sat_log_log (SAT_LOG_LEVEL_ERROR,
                     _("%s: Could not load config data from %s (%s)."),
                     __FUNCTION__, cfgfile, error->message);
        
        g_clear_error (&error);

        return;
    }

    /* debug message */
    sat_log_log (SAT_LOG_LEVEL_DEBUG,
                 _("%s: Reading configuration from %s"),
                 __FUNCTION__, cfgfile);

    /* set module name */
    buffer = g_path_get_basename (cfgfile);
    buffv = g_strsplit (buffer, ".mod", 0);
    module->name = g_strdup (buffv[0]);
    g_free (buffer);
    g_strfreev(buffv);

    /* get qth file */
    buffer = mod_cfg_get_str (module->cfgdata,
                              MOD_CFG_GLOBAL_SECTION,
                              MOD_CFG_QTH_FILE_KEY,
                              SAT_CFG_STR_DEF_QTH);

    confdir = get_user_conf_dir ();
    qthfile = g_strconcat (confdir, G_DIR_SEPARATOR_S, buffer, NULL);

    /* load QTH data */
    if (!qth_data_read (qthfile, module->qth)) {

        /* QTH file was not found for some reason */
        g_free (buffer);
        g_free (qthfile);

        /* remove cfg key */
        g_key_file_remove_key (module->cfgdata,
                               MOD_CFG_GLOBAL_SECTION,
                               MOD_CFG_QTH_FILE_KEY,
                               NULL);

        /* save modified cfg data to file */
        mod_cfg_save (module->name, module->cfgdata);

        /* try SAT_CFG_STR_DEF_QTH */
        buffer = sat_cfg_get_str (SAT_CFG_STR_DEF_QTH);
        qthfile = g_strconcat (confdir, G_DIR_SEPARATOR_S, buffer, NULL);

        if (!qth_data_read (qthfile, module->qth)) {

            sat_log_log (SAT_LOG_LEVEL_ERROR,
                         _("%s: Can not load default QTH file %s; using built-in defaults"),
                         __FUNCTION__, buffer);

            /* settings are really screwed up; we need some safe values here */
            module->qth->name = g_strdup (_("Error"));
            module->qth->loc  = g_strdup (_("Error"));
            module->qth->lat  = 0.0;
            module->qth->lon  = 0.0;
            module->qth->alt  = 0;
        }
    }

    g_free (buffer);
    g_free (confdir);
    g_free (qthfile);


    /* get timeout value */
    module->timeout = mod_cfg_get_int (module->cfgdata,
                                       MOD_CFG_GLOBAL_SECTION,
                                       MOD_CFG_TIMEOUT_KEY,
                                       SAT_CFG_INT_MODULE_TIMEOUT);

    /* get grid layout configuration (introduced in 1.2) */
    buffer = mod_cfg_get_str (module->cfgdata,
                              MOD_CFG_GLOBAL_SECTION,
                              MOD_CFG_GRID,
                              SAT_CFG_STR_MODULE_GRID);

    /* convert to an integer list */
    buffv = g_strsplit (buffer, ";", 0);
    length = g_strv_length (buffv);
    if ((length == 0) || (length % 5 != 0)) {
        /* the grid configuration is bogus; override with global default */
        sat_log_log (SAT_LOG_LEVEL_ERROR,
                     _("%s: Module layout is invalid: %s. Using default."),
                     __FUNCTION__, buffer);
        g_free (buffer);
        g_strfreev (buffv);

        buffer = sat_cfg_get_str_def (SAT_CFG_STR_MODULE_GRID);
        buffv = g_strsplit (buffer, ";", 0);
        length = g_strv_length (buffv);
    }

    /* make a debug log entry */
    sat_log_log (SAT_LOG_LEVEL_DEBUG,
                 _("%s: GRID(%d): %s"),
                 __FUNCTION__, length, buffer);
    g_free (buffer);

    /* number of views: we have five numbers per view (type,left,right,top,bottom) */
    module->nviews = length / 5;
    module->grid = g_try_new0 (gint, length);

    /* FIXME: we should check module->grid != NULL */

    /* convert chars to integers */
    for (i = 0; i < length; i++) {
        module->grid[i] = (gint) g_ascii_strtoll (buffv[i], NULL, 0);
        //g_print ("%d: %s => %d\n", i, buffv[i], module->grid[i]);
    }
    g_strfreev (buffv);

}


/** \brief Read satellites into memory.
 *
 * This function reads the list of satellites from the configfile and
 * and then adds each satellite to the hash table.
 */
static void
gtk_sat_module_load_sats      (GtkSatModule *module)
{
    gint   *sats = NULL;
    gsize   length;
    GError *error = NULL;
    guint   i;
    sat_t  *sat;
    guint  *key = NULL;
    guint   succ = 0;

    /* get list of satellites from config file; abort in case of error */
    sats = g_key_file_get_integer_list (module->cfgdata,
                                        MOD_CFG_GLOBAL_SECTION,
                                        MOD_CFG_SATS_KEY,
                                        &length,
                                        &error);
            
    if (error != NULL) {
        sat_log_log (SAT_LOG_LEVEL_ERROR,
                     _("%s: Failed to get list of satellites (%s)"),
                     __FUNCTION__, error->message);

        g_clear_error (&error);

        /* GLib API says nothing about the contents in case of error */
        if (sats) {
            g_free (sats);
        }

        return;
    }
            
    /* read each satellite into hash table */
    for (i = 0; i < length; i++) {

        sat = g_new (sat_t, 1);

        if (gtk_sat_data_read_sat (sats[i], sat)) {

            /* the satellite could not be read */
            sat_log_log (SAT_LOG_LEVEL_ERROR,
                         _("%s: Error reading data for #%d"),
                         __FUNCTION__, sats[i]);

            g_free (sat);
        }
        else {
            /* check whether satellite is already in list
               in order to avoid duplicates
            */

            key = g_new0 (guint, 1);
            *key = sats[i];

            if (g_hash_table_lookup (module->satellites, key) == NULL) {

                gtk_sat_data_init_sat (sat, module->qth);

                g_hash_table_insert (module->satellites,
                                     key,
                                     sat);

                succ++;

                sat_log_log (SAT_LOG_LEVEL_DEBUG,
                             _("%s: Read data for #%d"),
                             __FUNCTION__, sats[i]);


            }
            else {
                sat_log_log (SAT_LOG_LEVEL_WARN,
                             _("%s: Sat #%d already in list"),
                             __FUNCTION__, sats[i]);

                /* it is not needed in this case */
                g_free (sat);
            }

        }
    }

    sat_log_log (SAT_LOG_LEVEL_MSG,
                 _("%s: Read %d out of %d satellites"),
                 __FUNCTION__,
                 succ,
                 length);

    g_free (sats);

}


/** \brief Free satellite data
 * 
 * This function is called automatically for each satellite when
 * the hash table is destroyed.
 */
static void gtk_sat_module_free_sat (gpointer sat)
{
    gtk_sat_data_free_sat (SAT(sat));
}


/** \brief Module timeout callback.
 */
static gboolean
gtk_sat_module_timeout_cb     (gpointer module)
{
    GtkSatModule   *mod = GTK_SAT_MODULE (module);
    GtkWidget      *child;
    gboolean        needupdate = FALSE;
    GdkWindowState  state;
    gdouble         delta;
    guint           i;


    /* in docked state, update only if tab is visible */
    switch (mod->state) {

    case GTK_SAT_MOD_STATE_DOCKED:

        if (mod_mgr_mod_is_visible (GTK_WIDGET (module))) {
            needupdate = TRUE;
        }
        break;

    default:
        state = gdk_window_get_state (GDK_WINDOW (GTK_WIDGET (module)->window));

        if (state & GDK_WINDOW_STATE_ICONIFIED) {
            needupdate = FALSE;
        }
        else {
            needupdate = TRUE;
        }
        break;
    }

    if (needupdate) {

          if (g_mutex_trylock(mod->busy)==FALSE) {
               
        sat_log_log (SAT_LOG_LEVEL_WARN,
                     _("%s: Previous cycle missed it's deadline."),
                     __FUNCTION__);
          
        return TRUE;
          
          }

        mod->rtNow = get_current_daynum ();

        /* Update time if throttle != 0 */
        if (mod->throttle) {

            delta = mod->throttle * (mod->rtNow - mod->rtPrev);
            mod->tmgCdnum = mod->tmgPdnum + delta;

        }
        /* else nothing to do since tmg_time_set updates
           mod->tmgCdnum every time
        */

        /* time to update header? */
        mod->head_count++;
        if (mod->head_count == mod->head_timeout) {

            /* reset counter */
            mod->head_count = 0;
            
            update_header (mod);
        }

        /* time to update events? */
        if (mod->event_count == mod->event_timeout) {

            /* reset counter, this will make gtk_sat_module_update_sat
               recalculate events
            */
            mod->event_count = 0;
        }

        /* update satellite data */
        g_hash_table_foreach (mod->satellites,
                              gtk_sat_module_update_sat,
                              module);

        /* update children */
        for (i = 0; i < mod->nviews; i++) {
            child = GTK_WIDGET (g_slist_nth_data (mod->views, i));
            update_child (child, mod->tmgCdnum);
        }


        /* update satellite data (it may have got out of sync during child updates) */
        g_hash_table_foreach (mod->satellites,
                              gtk_sat_module_update_sat,
                              module);

        /* send notice to radio and rotator controller */
        if (mod->rigctrl)
            gtk_rig_ctrl_update (GTK_RIG_CTRL (mod->rigctrl), mod->tmgCdnum);
        if (mod->rotctrl)
            gtk_rot_ctrl_update (GTK_ROT_CTRL (mod->rotctrl), mod->tmgCdnum);
            
        /* check and update Sky at glance */
        /* FIXME: We should have some timeout counter to ensure that we don't
         * update GtkSkyGlance too often when running with high throttle values;
         * however, the update does not seem to add any significant load even
         * when running at max throttle
         */
        if (mod->skg)
            update_skg (mod);


        mod->event_count++;

        /* store time keeping variables */
        mod->rtPrev = mod->rtNow;
        mod->tmgPdnum = mod->tmgCdnum;

        if (mod->tmgActive) {

            /* update time control spin buttons when we are
               in RT or SRT mode */
            if (mod->throttle) {
                tmg_update_widgets (mod);
            }

        }

          g_mutex_unlock(mod->busy);

    }
    return TRUE;
}


/** \brief Update a child widget.
 *  \param child Pointer to the child widget (views)
 *  \param tstamp The current timestamp
 * 
 * This function is called by the main loop of the GtkSatModule widget for
 * each view in the layout grid.
 */
static void
update_child (GtkWidget *child, gdouble tstamp)
{
    if (IS_GTK_SAT_LIST(child)) {
        GTK_SAT_LIST (child)->tstamp = tstamp;
        gtk_sat_list_update (child);
    }

    else if (IS_GTK_SAT_MAP(child)) {
        GTK_SAT_MAP (child)->tstamp = tstamp;
        gtk_sat_map_update (child);
    }

    else if (IS_GTK_POLAR_VIEW(child)) {
        GTK_POLAR_VIEW (child)->tstamp = tstamp;
        gtk_polar_view_update (child);
    }

    else if (IS_GTK_SINGLE_SAT(child)) {
        GTK_SINGLE_SAT (child)->tstamp = tstamp;
        gtk_single_sat_update (child);
    }

    else if (IS_GTK_EVENT_LIST(child)) {
        GTK_EVENT_LIST (child)->tstamp = tstamp;
        gtk_event_list_update (child);
    }

    else {
        sat_log_log (SAT_LOG_LEVEL_BUG,
                     _("%f:%d: Unknown child type"),
                     __FILE__, __LINE__);
    }
}


/** \brief Update a given satellite.
 *  \param key The hash table key (catnum)
 *  \param val The hash table value (sat_t structure)
 *  \param data User data (the GtkSatModule widget).
 *
 * This function updates the tracking data for a given satelite. It is called by
 * the timeout handler for each element in the hash table.
 */
static void
gtk_sat_module_update_sat    (gpointer key, gpointer val, gpointer data)
{
    sat_t        *sat;
    GtkSatModule *module;
    gdouble       daynum;
    double        age;
    obs_set_t     obs_set = {0,0,0,0};
    geodetic_t    sat_geodetic = {0,0,0,0};
    geodetic_t    obs_geodetic = {0,0,0,0};
    gdouble       maxdt;


   g_return_if_fail ((val != NULL) && (data != NULL));

    sat = SAT(val);
    module = GTK_SAT_MODULE (data);


    /* get current time (real or simulated */
    daynum = module->tmgCdnum;

    /* update events if the event counter has been reset
       and the other requirements are fulfilled
    */
    if ((GTK_SAT_MODULE (module)->event_count == 0) &&
        (sat->otype != ORBIT_TYPE_GEO) &&
        (sat->otype != ORBIT_TYPE_DECAYED) &&
        has_aos (sat, module->qth))    {

        /* Note that has_aos may return TRUE for geostationary sats
           whose orbit deviate from a true-geostat orbit, however,
           find_aos and find_los will not go beyond the time limit
           we specify (in those cases they return 0.0 for AOS/LOS times.
           We use SAT_CFG_INT_PRED_LOOK_AHEAD for upper time limit
        */
        maxdt = (gdouble) sat_cfg_get_int (SAT_CFG_INT_PRED_LOOK_AHEAD);
        sat->aos = find_aos (sat, module->qth, daynum, maxdt);
        sat->los = find_los (sat, module->qth, daynum, maxdt);

    }


    /*** FIXME: we don't need to do this every time! */
    obs_geodetic.lon = module->qth->lon * de2ra;
    obs_geodetic.lat = module->qth->lat * de2ra;
    obs_geodetic.alt = module->qth->alt / 1000.0;
    obs_geodetic.theta = 0;


    sat->jul_utc = daynum;
    sat->tsince = (sat->jul_utc - sat->jul_epoch) * xmnpda;



    /* call the norad routines according to the deep-space flag */
    if (sat->flags & DEEP_SPACE_EPHEM_FLAG)
        SDP4 (sat, sat->tsince);
    else
        SGP4 (sat, sat->tsince);

    /* scale position and velocity to km and km/sec */
    Convert_Sat_State (&sat->pos, &sat->vel);

    /* get the velocity of the satellite */
    Magnitude (&sat->vel);
    sat->velo = sat->vel.w;
    Calculate_Obs (sat->jul_utc, &sat->pos, &sat->vel, &obs_geodetic, &obs_set);
    Calculate_LatLonAlt (sat->jul_utc, &sat->pos, &sat_geodetic);

    /*** FIXME: should we ensure sat_geodetic.lon stays between -pi and pi? */
    while (sat_geodetic.lon < -pi)
        sat_geodetic.lon += twopi;

    while (sat_geodetic.lon > (pi))
        sat_geodetic.lon -= twopi;

    sat->az = Degrees (obs_set.az);
    sat->el = Degrees (obs_set.el);
    sat->range = obs_set.range;
    sat->range_rate = obs_set.range_rate;
    sat->ssplat = Degrees (sat_geodetic.lat);
    sat->ssplon = Degrees (sat_geodetic.lon);
    sat->alt = sat_geodetic.alt;
    sat->ma = Degrees (sat->phase);
    sat->ma *= 256.0/360.0;
    sat->phase = Degrees (sat->phase);

    /* same formulas, but the one from predict is nicer */
    //sat->footprint = 2.0 * xkmper * acos (xkmper/sat->pos.w);
    sat->footprint = 12756.33 * acos (xkmper / (xkmper+sat->alt));
    age = sat->jul_utc - sat->jul_epoch;
    sat->orbit = (long) floor((sat->tle.xno * xmnpda/twopi +
                               age * sat->tle.bstar * ae) * age +
                              sat->tle.xmo/twopi) + sat->tle.revnum - 1;


    /*** FIXME: Squint + AOS / LOS code */
    /* ************************************************************************* */
    /* ************************************************************************* */
    /* ******************************* HACK ************************************ */
    /* ************************************************************************* */
    /* ************************************************************************* */

    /* OSC Data */
    if (sat_cfg_get_bool(SAT_CFG_BOOL_SEND_OSC) == TRUE) {
    //OSC
    GString *msg_header, *tlecatnr_string;
    msg_header = g_string_new("/gpredict/sat/");
    tlecatnr_string = g_strdup_printf("%i", sat->tle.catnr);
    g_string_append(msg_header,  tlecatnr_string);        

   	    lo_address t = lo_address_new(NULL, "7770");
	    if (lo_send(t,msg_header->str , "ffff",  sat->az, sat->el, sat->alt, sat->velo) == -1)
		    printf("OSC error %d: %s\n", lo_address_errno(t), lo_address_errstr(t));
	    lo_address_free (t);
    }

}



/** \brief Module options
 *
 * Invoke module-wide popup menu
 */
static void
gtk_sat_module_popup_cb       (GtkWidget *button, gpointer data)
{
    gtk_sat_module_popup (GTK_SAT_MODULE (data));
}


/** \brief Close module.
 *  \param button The button widget that received the signal.
 *  \param data Pointer the GtkSatModule widget, which should be destroyed.
 *
 * This function is called when the user clicks on the "close" minibutton.
 * The functions checks the state of the module. If the module is docked
 * it is removed from the mod-mgr notebook whereafter it is destroyed.
 * if the module is either in undocked or fullscreen state, the parent
 * window is destroyed, which will automatically destroy the module as
 * well.
 *
 * NOTE: Don't use button, since we don't know what kind of widget it is
 *       (it may be button or menu item).
 */
void
gtk_sat_module_close_cb       (GtkWidget *button, gpointer data)
{
    GtkSatModule *module = GTK_SAT_MODULE (data);
    gchar        *name;
    gint          retcode;

    name = g_strdup (module->name);

    sat_log_log (SAT_LOG_LEVEL_DEBUG,
                 _("%s: Module %s recevied CLOSE signal."),
                 __FUNCTION__, name);

    /* save configuration to ensure that dynamic data like state is stored */
    mod_cfg_save (module->name, module->cfgdata);

    switch (module->state) {

    case GTK_SAT_MOD_STATE_DOCKED:
        sat_log_log (SAT_LOG_LEVEL_DEBUG,
                     _("%s: Module %s is in DOCKED state."),
                     __FUNCTION__, name);

        retcode = mod_mgr_remove_module (GTK_WIDGET (module));

        if (retcode) {
            sat_log_log (SAT_LOG_LEVEL_BUG,
                         _("%s: Module %s was not found in mod-mgr (%d)\n"\
                           "Internal state is corrupt?"),
                         __FUNCTION__, name, retcode);
        }

        break;

    case GTK_SAT_MOD_STATE_WINDOW:
        sat_log_log (SAT_LOG_LEVEL_DEBUG,
                     _("%s: Module %s is in WINDOW state."),
                     __FUNCTION__, name);

        retcode = mod_mgr_remove_module (GTK_WIDGET (module));

        if (retcode) {
            sat_log_log (SAT_LOG_LEVEL_BUG,
                         _("%s: Module %s was not found in mod-mgr (%d)\n"\
                           "Internal state is corrupt?"),
                         __FUNCTION__, name, retcode);
        }

        /* increase referene count */
        g_object_ref (module);

        /* remove module from window, destroy window */
        gtk_container_remove (GTK_CONTAINER (GTK_SAT_MODULE (module)->win),
                              GTK_WIDGET (module));
        gtk_widget_destroy (GTK_SAT_MODULE (module)->win);
        GTK_SAT_MODULE (module)->win = NULL;

        /* release module */
        g_object_unref (module);

        break;

    case GTK_SAT_MOD_STATE_FULLSCREEN:
        sat_log_log (SAT_LOG_LEVEL_DEBUG,
                     _("%s: Module %s is in FULLSCREEN state."),
                     __FUNCTION__, name);

        retcode = mod_mgr_remove_module (GTK_WIDGET (module));

        if (retcode) {
            sat_log_log (SAT_LOG_LEVEL_BUG,
                         _("%s: Module %s was not found in mod-mgr (%d)\n"\
                           "Internal state is corrupt?"),
                         __FUNCTION__, name, retcode);
        }


        /* increase referene count */
        g_object_ref (module);

        /* remove module from window, destroy window */
        gtk_container_remove (GTK_CONTAINER (GTK_SAT_MODULE (module)->win),
                              GTK_WIDGET (module));
        gtk_widget_destroy (GTK_SAT_MODULE (module)->win);
        GTK_SAT_MODULE (module)->win = NULL;

        /* release module */
        g_object_unref (module);

        break;

    default:
        sat_log_log (SAT_LOG_LEVEL_BUG,
                     _("%s: Module %s has unknown state: %d"),
                     __FUNCTION__, name, module->state);
        break;
    }

    /* appearantly, module will be destroyed when removed from notebook */
    /* gtk_widget_destroy (GTK_WIDGET (module)); */

    sat_log_log (SAT_LOG_LEVEL_MSG,
                 _("%s: Module %s closed."),
                 __FUNCTION__, name);

    g_free (name);
             
}



/** \brief Configure module.
 *  \param button The button widget that received the signal.
 *  \param data Pointer the GtkSatModule widget, which should be reconfigured
 *
 * This function is called when the user clicks on the "configure" minibutton.
 * The function incokes the mod_cfg_edit funcion, which has the same look and feel
 * as the dialog used to create a new module.
 *
 * NOTE: Don't use button, since we don't know what kind of widget it is
 *       (it may be button or menu item).
 */
void
gtk_sat_module_config_cb       (GtkWidget *button, gpointer data)
{
    GtkSatModule        *module = GTK_SAT_MODULE (data);
    GtkWidget           *toplevel;
    gchar               *name;
    gchar               *cfgfile;
    mod_cfg_status_t     retcode;
    gtk_sat_mod_state_t  laststate;
    gint w,h;


    if (module->win != NULL)
        toplevel = module->win;
    else
        toplevel = gtk_widget_get_toplevel (GTK_WIDGET (data));

    name = g_strdup (module->name);

    sat_log_log (SAT_LOG_LEVEL_DEBUG,
                 _("%s: Module %s recevied CONFIG signal."),
                 __FUNCTION__, name);

    /* stop timeout */
    if (!g_source_remove (module->timerid)) {
        /* internal error, since the timerid appears
           to be invalid.
        */
        sat_log_log (SAT_LOG_LEVEL_BUG,
                     _("%s: Could not stop timeout callback\n"\
                       "%s: Source ID %d seems invalid."),
                     __FUNCTION__, __FUNCTION__, module->timerid);

    }
    else {
        module->timerid = -1;

        retcode = mod_cfg_edit (name, module->cfgdata, toplevel);

        if (retcode == MOD_CFG_OK) {
            /* save changes */
            retcode = mod_cfg_save (name, module->cfgdata);

            if (retcode != MOD_CFG_OK) {

                /**** FIXME: dialog */
                sat_log_log (SAT_LOG_LEVEL_ERROR,
                             _("%s: Module configuration failed for some reason."),
                             __FUNCTION__);

                /* don't try to reload config since it may be
                   invalid; keep original
                */

            }
            else {

                /* store state and size */
                laststate = module->state;
                w = GTK_WIDGET (module)->allocation.width;
                h = GTK_WIDGET (module)->allocation.height;

                gtk_sat_module_close_cb (NULL, module);

                gchar *confdir = get_modules_dir ();
                cfgfile = g_strconcat (confdir, G_DIR_SEPARATOR_S, name, ".mod", NULL);
                g_free (confdir);

                module = GTK_SAT_MODULE (gtk_sat_module_new (cfgfile));
                module->state = laststate;
                
                switch (laststate) {

                case GTK_SAT_MOD_STATE_DOCKED:

                    /* re-open module by adding it to the mod-mgr */
                    mod_mgr_add_module (GTK_WIDGET (module), TRUE);

                    break;

                case GTK_SAT_MOD_STATE_WINDOW:

                    /* add to module manager */
                    mod_mgr_add_module (GTK_WIDGET (module), FALSE);

                    /* create window */
                    module->win = gtk_window_new (GTK_WINDOW_TOPLEVEL);
                    gtk_window_set_title (GTK_WINDOW (module->win),
                                          module->name);
                    gtk_window_set_default_size (GTK_WINDOW (module->win),
                                                 w, h);

                    /** FIXME: window icon and such */

                    /* add module to window */
                    gtk_container_add (GTK_CONTAINER (module->win),
                                       GTK_WIDGET (module));

                    /* show window */
                    gtk_widget_show_all (module->win);

                    break;

                case GTK_SAT_MOD_STATE_FULLSCREEN:

                    /* add to module manager */
                    mod_mgr_add_module (GTK_WIDGET (module), FALSE);

                    /* create window */
                    module->win = gtk_window_new (GTK_WINDOW_TOPLEVEL);
                    gtk_window_set_title (GTK_WINDOW (module->win),
                                          module->name);
                    gtk_window_set_default_size (GTK_WINDOW (module->win),
                                                 w, h);

                    /** FIXME: window icon and such */

                    /* add module to window */
                    gtk_container_add (GTK_CONTAINER (module->win),
                                       GTK_WIDGET (module));

                    /* show window */
                    gtk_widget_show_all (module->win);

                    gtk_window_fullscreen (GTK_WINDOW (module->win));

                    break;

                default:
                    sat_log_log (SAT_LOG_LEVEL_BUG,
                                 _("%s: Module %s has unknown state: %d"),
                                 __FUNCTION__, name, module->state);
                    break;
                }

                g_free (cfgfile);

            }

        }
        else {
            /* user cancelled => just re-start timer */
            module->timerid = g_timeout_add (module->timeout,
                                             gtk_sat_module_timeout_cb,
                                             data);
            
        }
    }

    g_free (name);

}


static void
update_header (GtkSatModule *module)
{
    gchar *fmtstr;
    time_t t;
    guint size;
    gchar buff[TIME_FORMAT_MAX_LENGTH+1];



    t = (module->tmgCdnum - 2440587.5)*86400.;

    fmtstr = sat_cfg_get_str (SAT_CFG_STR_TIME_FORMAT);
    
    /* format either local time or UTC depending on check box */
    if (sat_cfg_get_bool (SAT_CFG_BOOL_USE_LOCAL_TIME))
        size = strftime (buff, TIME_FORMAT_MAX_LENGTH, fmtstr, localtime (&t));
    else
        size = strftime (buff, TIME_FORMAT_MAX_LENGTH, fmtstr, gmtime (&t));

    if (size < TIME_FORMAT_MAX_LENGTH)
        buff[size]='\0';
    else
        buff[TIME_FORMAT_MAX_LENGTH]='\0';

    gtk_label_set_text (GTK_LABEL (module->header), buff);
    g_free (fmtstr);

    if (module->tmgActive)
        tmg_update_state (module);
}


static gboolean empty (gpointer key, gpointer val, gpointer data)
{
    /* TRUE => sat removed from hash table */
    return TRUE;
}


/** \brief Reload satellites.
 *  \param module Pointer to a GtkSatModule widget.
 *
 * This function is used to reload the satellites in a module. This is can be
 * useful when:
 *
 *   1. The TLE files have been updated.
 *   2. The module configuration has changed (i.e. which satellites to track).
 *
 * The function assumes that module->cfgdata has already been updated, and so
 * all it has to do is to free module->satellites and re-execute the satellite
 * loading sequence.
 */
void
gtk_sat_module_reload_sats    (GtkSatModule *module)
{
    GtkWidget *child;
    guint      i;


    g_return_if_fail (IS_GTK_SAT_MODULE (module));

    /* lock module */
    g_mutex_lock(module->busy);
     
    sat_log_log (SAT_LOG_LEVEL_MSG,
                 _("%s: Reloading satellites for module %s"),
                 __FUNCTION__, module->name);

    /* remove each element from the hash table, but keep the hash table */
    g_hash_table_foreach_remove (module->satellites, empty, NULL);

    /* reset event counter so that next AOS/LOS gets re-calculated */
    module->event_count = 0;       

    /* load satellites */
    gtk_sat_module_load_sats (module);

    /* update children */
    for (i = 0; i < module->nviews; i++) {
        child = GTK_WIDGET (g_slist_nth_data (module->views, i));
        reload_sats_in_child (child, module);
    }

    /* FIXME: radio and rotator controller */
    
    /* unlock module */
    g_mutex_unlock(module->busy);
}


/** \brief Reload satellites in view */
static void
reload_sats_in_child (GtkWidget *widget, GtkSatModule *module)
{
 
 
    if (IS_GTK_SINGLE_SAT (G_OBJECT (widget))) {
        gtk_single_sat_reload_sats (widget, module->satellites);
    }

    else if (IS_GTK_POLAR_VIEW (widget)) {
        gtk_polar_view_reload_sats (widget, module->satellites);
    }

    else if (IS_GTK_SAT_MAP (widget)) {
        gtk_sat_map_reload_sats (widget, module->satellites);
    }

    else if (IS_GTK_SAT_LIST (widget)) {
    }


    else {
        sat_log_log (SAT_LOG_LEVEL_BUG,
                     _("%f:%d: Unknown child type"),
                     __FILE__, __LINE__);
    }

}


/** \brief Re-configure module.
 *  \param module The module.
 *  \param local Flag indicating whether reconfiguration is requested from 
 *               local configuration dialog.
 *
 */
void gtk_sat_module_reconf (GtkSatModule *module, gboolean local)
{
}


/** \brief Calculate the layout grid size.
  * \param module Pointer to the GtkSatModule widget.
  * \param rows Return value for number of rows
  * \param cols Return value for number of columns
  *
  * It is assumed that module->grid and module->nviews have chierent values.
  */
static void get_grid_size (GtkSatModule *module, guint *rows, guint *cols)
{
    guint i;
    guint xmax = 0;
    guint ymax = 0;


    for (i = 0; i < module->nviews; i++) {
        xmax = MAX(xmax,module->grid[5*i+2]);
        ymax = MAX(ymax,module->grid[5*i+4]);
    }

    *cols = xmax;
    *rows = ymax;
}


/** \brief Update GtkSkyGlance view
 *  \param module Pointer to the GtkSatModule widget
 * 
 * This function checks how long ago the GtkSkyGlance widget has been updated
 * and performs an update if necessary. The current timeout is set to 60 sec.
 * 
 * This is a cheap/lazy implementation of automatic update. Instead of
 * performing a real update by "moving" the objects on the GtkSkyGlance canvas,
 * we simply replace the current GtkSkyGlance object with a new one.
 * Ugly but safe.
 * 
 * To ensure smooth performance while running in simulated real time with high
 * throttle value or manual time mode, the caller is responsible for only calling
 * this function at an appropriate frequency (e.g. every 10 cycle).
 */
static void update_skg (GtkSatModule *module)
{
    
    /* threshold is ~60 seconds */
    if G_UNLIKELY(fabs(module->tmgCdnum - module->lastSkgUpd) > 7.0e-4) {
        
        sat_log_log (SAT_LOG_LEVEL_MSG,
                     _("%s: Updating GtkSkyGlance for %s"),
                     __FUNCTION__, module->name);
        
        gtk_container_remove (GTK_CONTAINER (module->skgwin), module->skg);
        module->skg = gtk_sky_glance_new (module->satellites, module->qth, module->tmgCdnum);
        gtk_container_add (GTK_CONTAINER (module->skgwin), module->skg);
        gtk_widget_show_all (module->skg);
        
        module->lastSkgUpd = module->tmgCdnum;
    }
}
