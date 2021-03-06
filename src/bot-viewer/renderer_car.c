#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <GL/gl.h>
#include <GL/glu.h>
#include <gdk/gdkkeysyms.h>

#include <lcm/lcm.h>

#include <bot/bot_core.h>
#include <bot/gtk/gtk_util.h>
#include <bot/viewer/viewer.h>
#include <bot/gl/gl_util.h>

#include <common/geometry.h>
#include <common/globals.h>
#include <common/fbconf.h>

#include <bot/viewer/rwx.h>

#include <lcmtypes/fblcm.h>

#ifndef MODELS_DIR
#error "MODELS_DIR is not defined!"
#endif

#define RENDERER_NAME "Car"

#define PARAM_NAME_BLING "Draw detailed car model"
#define PARAM_NAME_WHEELS "Draw wheels"

#define PARAM_FOLLOW_POS "Follow position"
#define PARAM_FOLLOW_YAW "Follow yaw"
#define PARAM_MAXPOSES "Max poses"

#define MAX_POSES   10000

#ifndef sq
#define sq(a) ((a)*(a))
#endif

#define to_degrees(rad) ((rad)*180/M_PI)

typedef struct _RendererCar {
    Renderer renderer;
    EventHandler ehandler;

    ATrans *atrans;
    lcm_t *lcm;
    BotConf * config;

    rwx_model_t *chassis_model;
    rwx_model_t *wheel_model;
    int display_lists_ready;
    GLuint chassis_dl;
    GLuint wheel_dl;

    Viewer          *viewer;
    BotGtkParamWidget *pw;
    BotPtrCircular    *path; // elements: double[3] 
    botlcm_pose_t          last_pose;
    int             max_draw_poses;

    int          display_detail;
    double       last_xy[2]; // last mouse press

    int          did_teleport; // have we done our initial teleport?
    // for teleport mode
    int          teleport_car;
    int          teleport_car_request;

    float        wheelspeeds[4];
    float        wheelpos[4];
    float        wheel_angle;
    float        wheel_heights[4];

    pointlist2d_t *footprint;
} RendererCar;

#define DETAIL_NONE 0
#define DETAIL_SPEED 1
#define DETAIL_RPY 2
#define NUM_DETAILS 3

static void load_bling (RendererCar *self);

// called by bot_ptr_circular when a path sample needs to be freed.
static void free_path_element(void *user, void *p)
{
    free(p);
}

static void on_find_button(GtkWidget *button, RendererCar *self)
{
    ViewHandler *vhandler = self->viewer->view_handler;

    double eye[3];
    double lookat[3];
    double up[3];

    vhandler->get_eye_look(vhandler, eye, lookat, up);
    double diff[3];
    bot_vector_subtract_3d(eye, lookat, diff);

    double pos[3] = { 0, 0, 0 };
    atrans_vehicle_pos_local(self->atrans, pos);

    bot_vector_add_3d(pos, diff, eye);

    vhandler->set_look_at(vhandler, eye, pos, up);

    viewer_request_redraw(self->viewer);
}

static void
on_pose(const lcm_recv_buf_t *rbuf, const char *channel, 
        const botlcm_pose_t *pose, void *user_data)
{
    RendererCar *self = (RendererCar*) user_data;
    ViewHandler *vhandler = self->viewer->view_handler;

    double lastpos[3] = {0,0,0};
    if (bot_ptr_circular_size(self->path))
        memcpy(lastpos, bot_ptr_circular_index(self->path, 0),
               3 * sizeof(double));

    double diff[3];
    bot_vector_subtract_3d(pose->pos, lastpos, diff);

    if (bot_vector_magnitude_3d(diff) > 2.0) {
        // clear the buffer if we jump
        bot_ptr_circular_clear(self->path);
    }

    if (bot_vector_magnitude_3d(diff) > 0.1 ||
            bot_ptr_circular_size(self->path)==0) {
        double *p = (double*) calloc(3, sizeof(double));
        memcpy(p, pose->pos, sizeof(double)*3);
        bot_ptr_circular_add(self->path, p);
    }

    if (vhandler && vhandler->update_follow_target && !self->teleport_car) {
        vhandler->update_follow_target(vhandler, pose->pos, pose->orientation);
    }

    if (!self->did_teleport)
        on_find_button(NULL, self);
    self->did_teleport = 1;

    int64_t dt = pose->utime - self->last_pose.utime;
    double r = bot_conf_get_double_or_default (self->config,
            "renderer_car.wheel_radius", 0.3);

    if (self->last_pose.utime) {
        int i;
        for (i = 0; i < 4; i++) {
            self->wheelpos[i] += self->wheelspeeds[i] * dt * 1e-6 / r;
            self->wheelpos[i] = bot_mod2pi (self->wheelpos[i]);
        }
    }
    memcpy (&self->last_pose, &pose, sizeof (botlcm_pose_t));

    viewer_request_redraw(self->viewer);
}

static void
car_free (Renderer *super)
{
    RendererCar *self = (RendererCar*) super->user;

    if (self->chassis_model)
        rwx_model_destroy (self->chassis_model);
    if (self->wheel_model)
        rwx_model_destroy (self->wheel_model);
    if (self->chassis_dl)
        glDeleteLists (self->chassis_dl, 1);
    if (self->wheel_dl)
        glDeleteLists (self->wheel_dl, 1);
    free (self);
}

static void
draw_wheels (RendererCar * self)
{
    glEnable (GL_BLEND);
    glEnable (GL_RESCALE_NORMAL);
    glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glShadeModel (GL_SMOOTH);

    glEnable (GL_LIGHTING);

    float wb = bot_conf_get_double_or_default (self->config,
            "renderer_car.wheel_base", 2);
    float ww = bot_conf_get_double_or_default (self->config,
            "renderer_car.wheel_track", 1);
    float r = bot_conf_get_double_or_default (self->config,
            "renderer_car.wheel_radius", 0.3);

    float xoff[4] = { wb, wb, 0, 0 };
    float yoff[4] = { ww/2, -ww/2, ww/2, -ww/2 };
    float yscale[4] = { 1, -1, 1, -1 };

    int i;
    for (i = 0; i < 4; i++) {
        glPushMatrix ();
        glTranslatef (xoff[i], yoff[i], -self->wheel_heights[i] + r);
        if (i < 2)
            glRotatef (self->wheel_angle * 180 / M_PI, 0, 0, 1);
        glScalef (1, yscale[i], 1);
        glRotatef (self->wheelpos[i] * 180 / M_PI, 0, 1, 0);
        glCallList (self->wheel_dl);
        glPopMatrix ();
    }
}

static void
draw_chassis_model (RendererCar * self)
{
    glPushMatrix ();
    if (self->ehandler.hovering) {
        glColor4f(0,1,1,.5);
        glEnable(GL_BLEND);
        glBegin (GL_QUADS);
        double x0=-2, x1=5;
        double y0=-2, y1=2;
        glVertex2f(x0, y0);
        glVertex2f(x0, y1);
        glVertex2f(x1, y1);
        glVertex2f(x1, y0);
        glEnd();
    }

    glEnable (GL_BLEND);
    glEnable (GL_RESCALE_NORMAL);
    glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glShadeModel (GL_SMOOTH);

    glEnable (GL_LIGHTING);
    glCallList (self->chassis_dl);
    glPopMatrix ();
}

static void
draw_footprint (RendererCar * self)
{
    assert (4 == self->footprint->npoints);

    glPushMatrix ();
    glLineWidth(2);
    glColor4f (1, 1, 1, .3);
    glEnable(GL_BLEND);
    glBegin (GL_QUADS);
    for (int i=0; i<4; i++)
        glVertex2f (self->footprint->points[i].x, 
                    self->footprint->points[i].y);
    glEnd ();

    glColor4f (1, 1, 1, 1);
    glBegin (GL_LINE_LOOP);
    for (int i=0; i<4; i++)
        glVertex2f (self->footprint->points[i].x, 
                    self->footprint->points[i].y);
    glEnd();

    point2d_t fp_centroid = { 0, 0 };
    geom_simple_polygon_centroid_2d (self->footprint, &fp_centroid);
    glTranslatef (fp_centroid.x, fp_centroid.y, .001);

    point2d_t fl = self->footprint->points[0];
    point2d_t fr = self->footprint->points[1];
    point2d_t br = self->footprint->points[2];

    double fp_length = fr.x - br.x;
    double fp_width = fabs (fr.y - fl.y);

    bot_gl_draw_arrow_2d (fp_length, fp_width, fp_length * 0.3, 
            fp_width * 0.5, self->ehandler.hovering);
    glPopMatrix ();
}

static void 
car_draw (Viewer *viewer, Renderer *super)
{
    RendererCar *self = (RendererCar*) super->user;

    botlcm_pose_t pose;
    if (!atrans_get_local_pose (self->atrans, &pose))
        return;

    BotGtkParamWidget *pw = self->pw;
    int bling = self->chassis_model ?
        bot_gtk_param_widget_get_bool (pw, PARAM_NAME_BLING) : 0;
    int wheels = self->wheel_model ?
        bot_gtk_param_widget_get_bool (pw, PARAM_NAME_WHEELS) : 0;
    if ((bling || wheels) && !self->display_lists_ready)  {
        load_bling (self);
    }

    glColor4f(0,1,0,0.75);
    glLineWidth (2);
    glBegin(GL_LINE_STRIP);
    glVertex3dv (self->last_pose.pos);
    for (unsigned int i = 0;
            i < MIN (bot_ptr_circular_size(self->path), self->max_draw_poses);
            i++) {
        glVertex3dv(bot_ptr_circular_index(self->path, i));
    }
    glEnd();

    glPushMatrix();

    // compute the rotation matrix to orient the vehicle in world
    // coordinates
    double body_quat_m[16];
    bot_quat_pos_to_matrix(pose.orientation, pose.pos, body_quat_m);

    // opengl expects column-major matrices
    double body_quat_m_opengl[16];
    bot_matrix_transpose_4x4d (body_quat_m, body_quat_m_opengl);

    // rotate and translate the vehicle
    glMultMatrixd (body_quat_m_opengl);

    glEnable (GL_DEPTH_TEST);

    if (bling && self->display_lists_ready && self->chassis_dl)
        draw_chassis_model (self);
    else
        draw_footprint (self);

    if (wheels && self->display_lists_ready && self->wheel_dl)
        draw_wheels (self);

    glPopMatrix();
    
    if (self->display_detail) {
        char buf[256];
        switch (self->display_detail) 
        {
        case DETAIL_SPEED:
            sprintf(buf, "%.2f m/s",
                    sqrt(sq(pose.vel[0]) + sq(pose.vel[1]) + sq(pose.vel[2])));
            break;
        case DETAIL_RPY:
        {
            double rpy[3];
            bot_quat_to_roll_pitch_yaw(pose.orientation, rpy);
            sprintf(buf, "r: %6.2f\np: %6.2f\ny: %6.2f", to_degrees(rpy[0]), to_degrees(rpy[1]), to_degrees(rpy[2]));
            break;
        }
}
        glColor3f(1,1,1);
        bot_gl_draw_text(pose.pos, GLUT_BITMAP_HELVETICA_12, buf,
                         BOT_GL_DRAW_TEXT_DROP_SHADOW);
    }
}

static void on_clear_button(GtkWidget *button, RendererCar *self)
{
    bot_ptr_circular_clear(self->path);

    viewer_request_redraw(self->viewer);
}

static void
on_param_widget_changed (BotGtkParamWidget *pw, const char *name, void *user)
{
    RendererCar *self = (RendererCar*) user;
    Viewer *viewer = self->viewer;

    viewer->view_handler->follow_mode = 0;
    if (bot_gtk_param_widget_get_bool(pw, PARAM_FOLLOW_POS))
        viewer->view_handler->follow_mode |= FOLLOW_POS;
    if (bot_gtk_param_widget_get_bool(pw, PARAM_FOLLOW_YAW))
        viewer->view_handler->follow_mode |= FOLLOW_YAW;

    self->max_draw_poses = bot_gtk_param_widget_get_int(pw, PARAM_MAXPOSES);

    viewer_request_redraw ( self->viewer);
}

static GLuint
compile_display_list (RendererCar * self, char * prefix, rwx_model_t * model)
{
    GLuint dl = glGenLists (1);
    glNewList (dl, GL_COMPILE);
    char key[64];

    sprintf (key, "renderer_car.%s_scale", prefix);
    double scale;
    if (bot_conf_get_double (self->config, key, &scale) == 0)
        glScalef (scale, scale, scale);
    
    sprintf (key, "renderer_car.%s_xyz_rotate", prefix);
    double rot[3];
    if (bot_conf_get_double_array (self->config, key, rot, 3) == 3) {
        glRotatef (rot[2], 0, 0, 1);
        glRotatef (rot[1], 0, 1, 0);
        glRotatef (rot[0], 1, 0, 0);
    }

    sprintf (key, "renderer_car.%s_translate", prefix);
    double trans[3];
    if (bot_conf_get_double_array (self->config, key, trans, 3) == 3)
        glTranslated (trans[0], trans[1], trans[2]);

    glEnable (GL_LIGHTING);
    rwx_model_gl_draw (model);
    glDisable (GL_LIGHTING);

    glEndList ();
    return dl;
}

static void
load_bling (RendererCar *self)
{
    if (!self->display_lists_ready) {
        if (self->chassis_model)
            self->chassis_dl = compile_display_list (self, "chassis",
                    self->chassis_model);
        if (self->wheel_model)
            self->wheel_dl = compile_display_list (self, "wheel",
                    self->wheel_model);
    }
    self->display_lists_ready = 1;
}

static int mouse_press (Viewer *viewer, EventHandler *ehandler,
                        const double ray_start[3], const double ray_dir[3], 
                        const GdkEventButton *event)
{
    RendererCar *self = (RendererCar*) ehandler->user;

    // only handle mouse button 1.
    if (self->teleport_car_request&&(event->button == 1)) {   
        self->teleport_car = 1;
    }


    if (event->type == GDK_2BUTTON_PRESS) {
        self->display_detail = (self->display_detail + 1) % NUM_DETAILS;
        viewer_request_redraw(self->viewer);
    }

    double carpos[3] = { 0, 0, 0 };
    atrans_vehicle_pos_local (self->atrans, carpos);
    geom_ray_z_plane_intersect_3d(POINT3D(ray_start), 
            POINT3D(ray_dir), carpos[2], POINT2D(self->last_xy));

    return 0;
}

static int mouse_release(Viewer *viewer, EventHandler *ehandler,
                         const double ray_start[3], const double ray_dir[3], 
                         const GdkEventButton *event)
{
    RendererCar *self = (RendererCar*) ehandler->user;

    self->teleport_car = 0;
    self->teleport_car_request = 0;
    self->ehandler.picking = 0;

    if (self->teleport_car && viewer->view_handler->follow_mode) {
        on_find_button(NULL, self);
    }

    return 0;
}

static int mouse_motion (Viewer *viewer, EventHandler *ehandler,
                         const double ray_start[3], const double ray_dir[3], 
                         const GdkEventMotion *event)
{
    RendererCar *self = (RendererCar*) ehandler->user;

    if (!self->teleport_car)
        return 0;

    if (!self->ehandler.picking)
        return 0;

    botlcm_pose_t p;
    if (atrans_get_local_pose (self->atrans, &p) < 0)
        return 0;

    double xy[2];
    geom_ray_z_plane_intersect_3d(POINT3D(ray_start), 
            POINT3D(ray_dir), p.pos[2], POINT2D(xy));
    
    int shift = event->state & GDK_SHIFT_MASK;

    if (shift) {
        // rotate!
        double theta1 = atan2(xy[1] - p.pos[1], xy[0] - p.pos[0]);

        // local
        double dq[4] = { cos (theta1/2), 0, 0, sin(theta1/2) };
        memcpy(p.orientation, dq, 4 * sizeof(double));
    } else {
        // translate
        p.pos[0] = xy[0];
        p.pos[1] = xy[1];
    }

    self->last_xy[0] = xy[0];
    self->last_xy[1] = xy[1];

    botlcm_pose_t_publish(self->lcm, "SIM_TELEPORT", &p);
    return 1;
}


static int key_press (Viewer *viewer, EventHandler *ehandler, 
        const GdkEventKey *event)
{
    RendererCar *self = (RendererCar*) ehandler->user;

    if (event->keyval == 't' || event->keyval == 'T') {
        viewer_request_pick(viewer, ehandler);
        self->teleport_car_request = 1;
        return 1;
    }
    if (event->keyval == GDK_Escape) {
        ehandler->picking = 0;
        self->teleport_car = 0;
        self->teleport_car_request = 0;
    }

    return 0;
}

static double pick_query(Viewer *viewer, EventHandler *ehandler, const double ray_start[3], const double ray_dir[3])
{
    RendererCar *self = (RendererCar*) ehandler->user;

    botlcm_pose_t pose;
    if (atrans_get_local_pose (self->atrans, &pose) < 0)
        return -1;

    double ray_start_body[3];
    bot_vector_subtract_3d (ray_start, pose.pos, ray_start_body);
    bot_quat_rotate_rev (pose.orientation, ray_start_body);

    double ray_dir_body[3] = { ray_dir[0], ray_dir[1], ray_dir[2] };
    bot_quat_rotate_rev (pose.orientation, ray_dir_body);
    bot_vector_normalize_3d (ray_dir_body);

    point3d_t car_pos_body = { 1.3, 0, 1 };
    point3d_t box_size = { 4.6, 2, 1.4 };
    double t = geom_ray_axis_aligned_box_intersect_3d (POINT3D(ray_start_body), 
            POINT3D (ray_dir_body), &car_pos_body, &box_size, NULL);
    if (isfinite (t)) return t;

    self->ehandler.hovering = 0;
    return -1;
}

static void
on_load_preferences (Viewer *viewer, GKeyFile *keyfile, void *user_data)
{
    RendererCar *self = user_data;
    bot_gtk_param_widget_load_from_key_file (self->pw, keyfile, RENDERER_NAME);
}

static void
on_save_preferences (Viewer *viewer, GKeyFile *keyfile, void *user_data)
{
    RendererCar *self = user_data;
    bot_gtk_param_widget_save_to_key_file (self->pw, keyfile, RENDERER_NAME);
}

void setup_renderer_car(Viewer *viewer, int render_priority)
{
    RendererCar *self = (RendererCar*) calloc (1, sizeof (RendererCar));

    Renderer *renderer = &self->renderer;

    renderer->draw = car_draw;
    renderer->destroy = car_free;

    renderer->widget = gtk_vbox_new(FALSE, 0);
    renderer->name = RENDERER_NAME;
    renderer->user = self;
    renderer->enabled = 1;

    EventHandler *ehandler = &self->ehandler;
    ehandler->name = RENDERER_NAME;
    ehandler->enabled = 1;
    ehandler->pick_query = pick_query;
    ehandler->key_press = key_press;
    ehandler->hover_query = pick_query;
    ehandler->mouse_press = mouse_press;
    ehandler->mouse_release = mouse_release;
    ehandler->mouse_motion = mouse_motion;
    ehandler->user = self;

    self->viewer = viewer;
    self->lcm = globals_get_lcm ();
    self->config = globals_get_config ();

    self->pw = BOT_GTK_PARAM_WIDGET(bot_gtk_param_widget_new());
    self->atrans = globals_get_atrans ();
    self->path = bot_ptr_circular_new (MAX_POSES, free_path_element, NULL);

    gtk_box_pack_start(GTK_BOX(renderer->widget), GTK_WIDGET(self->pw), TRUE, 
            TRUE, 0);

    bot_gtk_param_widget_add_booleans (self->pw, 0, PARAM_FOLLOW_POS, 1, NULL);
    bot_gtk_param_widget_add_booleans (self->pw, 0, PARAM_FOLLOW_YAW, 0, NULL);

    char * model;
    char path[256];
    if (bot_conf_get_str (self->config, "renderer_car.chassis_model",
                &model) == 0) {
        snprintf (path, sizeof (path), "%s/%s", MODELS_DIR, model);
        self->chassis_model = rwx_model_create (path);
    }
    if (bot_conf_get_str (self->config, "renderer_car.wheel_model",
                &model) == 0) {
        snprintf (path, sizeof (path), "%s/%s", MODELS_DIR, model);
        self->wheel_model = rwx_model_create (path);
    }

    if (self->chassis_model)
        bot_gtk_param_widget_add_booleans (self->pw, 0, PARAM_NAME_BLING, 1,
                NULL);
    if (self->wheel_model)
        bot_gtk_param_widget_add_booleans (self->pw, 0, PARAM_NAME_WHEELS, 1,
                NULL);
 
    self->max_draw_poses = 1000;
    bot_gtk_param_widget_add_int (self->pw, PARAM_MAXPOSES, 
            BOT_GTK_PARAM_WIDGET_SLIDER, 0, MAX_POSES, 100, self->max_draw_poses);
 
    GtkWidget *find_button = gtk_button_new_with_label("Find");
    gtk_box_pack_start(GTK_BOX(renderer->widget), find_button, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(find_button), "clicked", 
            G_CALLBACK (on_find_button), self);

    GtkWidget *clear_button = gtk_button_new_with_label("Clear path");
    gtk_box_pack_start(GTK_BOX(renderer->widget), clear_button, FALSE, FALSE, 
            0);
    g_signal_connect(G_OBJECT(clear_button), "clicked", 
            G_CALLBACK (on_clear_button), self);

    gtk_widget_show_all(renderer->widget);

    g_signal_connect (G_OBJECT (self->pw), "changed", 
                      G_CALLBACK (on_param_widget_changed), self);
    on_param_widget_changed(self->pw, "", self);

    botlcm_pose_t_subscribe(self->lcm, "POSE", on_pose, self);


    viewer_add_renderer(viewer, &self->renderer, render_priority);
    viewer_add_event_handler(viewer, &self->ehandler, render_priority);

    self->footprint = pointlist2d_new (4);
    fbconf_get_vehicle_footprint (self->config, 
            (double*)self->footprint->points);

    g_signal_connect (G_OBJECT (viewer), "load-preferences", 
            G_CALLBACK (on_load_preferences), self);
    g_signal_connect (G_OBJECT (viewer), "save-preferences",
            G_CALLBACK (on_save_preferences), self);
}
