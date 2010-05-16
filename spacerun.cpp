/**
  spacerun.cpp - Space Run

  This pile of hacks was written for Reddit Game Jam, May 16 2010 over a period
  of less than six hours. You may find it suspiciously similar to an ancient
  game called SkyRoads. Enjoy.

  Thanks to ekilfoil for building the 64-bit Linux build.

  Copyright (C) 2010 Nicholas Fraser

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

#if defined(_WIN32)
  #define GLFW_DLL 1
  #define WIN32_LEAN_AND_MEAN 1
  #include <windows.h>
  #include <malloc.h>
#endif

#include <cstdlib>
#include <cstdio>
#include <time.h>
#include <math.h>
#include <float.h>
#include <memory.h>

// platform workarounds (you need to get glut.h for windows)
#if defined(_WIN32)
  #define GLEXT_64_TYPES_DEFINED
  #pragma comment(lib, "opengl32.lib")
  #pragma comment(lib, "glu32.lib")
  #include <GL/gl.h>
  #include "glut.h"
  #define snprintf _snprintf
  static inline float roundf(float val) {   
    return floor(val + 0.5);
  }
#elif defined(__APPLE__)
  #include <OpenGL/gl.h>
  //#include <OpenGL/glext.h>
  #include <GLUT/glut.h>
#else
  #include <GL/gl.h>
  //#include <GL/glext.h>
  #include <GL/glut.h>
#endif

int w, h;
const float frame_time = 1.0f / 60.0f;

// game state machine
typedef enum State {
  Start,
  Flying,
  Exploding,
  Dead
} State;
State state;

// background stars
typedef struct Star {
  float x;
  float y;
  float size;
  float distance;
} Star;
#define star_count 100
Star stars[star_count];

// particle system. (particles list is a circular buffer; they all fade at the same speed)
typedef struct Particle {
  float x, y, z;
  float x_speed, y_speed, z_speed;
  float size, fade;
  unsigned char red, green, blue;
} Particle;
#define particle_count 100
Particle particles[particle_count];
int last_particle;
const float fade_speed = 2.0f;

// particle constants
const float engine_xy_speed_max = 0.25f;
const float engine_z_speed = -1.5f;
const float engine_size = 0.1f;
const float explode_speed = 1.0f;
const float explode_size = 0.15f;

// a track segment. there is one 'correct' tile in each row, which
// generates a valid path through the track.
typedef struct Segment {
  unsigned char live, correct;
  unsigned char red, green, blue;
} Segment;

// the track (as a circular buffer, row indexed by current_row)
#define cols 5
#define rows 40
Segment track[rows][cols];
int current_row;

// current speed and position
float z;
float x;
int current_col;
float speed, accel;
const float accel_start = 3.0f;
const float accel_min = 0.2f;
const float accel_mult = 0.98f;
const float decel_mult = 0.99f;
const float decel_linear = 0.05f;
const float drift_mult = 10.0f;
const float drift_linear = 1.0f;
const float explode_fade_speed = 2.0f; // speed at which exploding ship starts fading out

// score
float score;
const float score_mult = 2.0f;

// segment dimensions
static const float segment_width  = 1.0f;
static const float segment_depth  = 1.5f;
static const float segment_height = 0.2f;

// gets a random float between -1 and 1
float rand_float(void) {
  return (rand() % 1000) / 1000.0f * 2.0f - 1.0f;
}

// gets a random float in the given range
float rand_float_range(float min, float max) {
  return (rand() % 1000) / 1000.0f * (max - min) + min;
}

// randomize a segment color
void randomize_colour(Segment* square) {
  square->red   = 127 + rand() % 128;
  square->green = 127 + rand() % 128;
  square->blue  = 127 + rand() % 128;
}

// randomize a star
void randomize_star(Star* star) {
  star->x = rand_float_range(0.0f, 1.0f);
  star->y = rand_float_range(0.0f, 1.0f);
  star->size = rand_float_range(1.0f, 3.0f);
  star->distance = rand_float_range(3.0f, 10.0f);
}

// prints a row to the console (for debugging purposes)
void print_row(int row) {
  printf("current col: %i row: |", current_col);
  for (int col = 0; col < cols; ++col)
    putchar(track[row][col].correct ? 'C' : (track[row][col].live ? 'x' : ' '));
  printf("|\n");
}

// emits a particle
void emit(Particle* particle) {
  last_particle = (last_particle + 1) % particle_count;
  memcpy(&particles[last_particle], particle, sizeof(*particle));
}

// add a new row. this function contains all the logic for generating a valid track without gaps.
void new_row() {
  int last_row = (current_row + rows - 1) % rows;

  // randomize live tiles.
  for (int col = 0; col < cols; ++col) {
    track[current_row][col].live = rand() % 2;
    track[current_row][col].correct = false;
  }

  // figure out the correct column.
  int correct_col = -1;
  for (int col = 0; col < cols; ++col) {
    if (track[last_row][col].correct) {
      correct_col = col;
      break;
    }
  }

  const char* correct_str = "";

  // if the column is still live, it stays correct. otherwise see if we can
  // switch. if we can't, force the column to be live.
  if (track[current_row][correct_col].live) {
    track[current_row][correct_col].correct = true;
    correct_str = "staying in current column";
  } else {
    if (correct_col > 0 && track[last_row][correct_col - 1].live && track[current_row][correct_col - 1].live) {
      track[current_row][correct_col - 1].correct = true;
      correct_str = "going left";
    } else if (correct_col < cols - 1 && track[last_row][correct_col + 1].live && track[current_row][correct_col + 1].live) {
      track[current_row][correct_col + 1].correct = true;
      correct_str = "going right";
    } else {
      track[current_row][correct_col].live = track[current_row][correct_col].correct = true;
      correct_str = "forcing stay";
    }
  }

  // colors must match in columns; if we start a new column, we randomize.
  for (int col = 0; col < cols; ++col) {
    if (track[last_row][col].live) {
      track[current_row][col].red   = track[last_row][col].red;
      track[current_row][col].green = track[last_row][col].green;
      track[current_row][col].blue  = track[last_row][col].blue;
    } else {
      randomize_colour(&track[current_row][col]);
    }
  }
  
  /*
  printf("new row:");
  for (int col = 0; col < cols; ++col)
    putchar(track[current_row][col].correct ? 'C' : (track[current_row][col].live ? 'x' : ' '));
  printf(" %s\n", correct_str);
  */

  // advance track pos
  current_row = (current_row + 1) % rows;
}

// start a new game
void new_game(void) {
  state = Start;
  score = 0;

  // randomize starfield
  for (int i = 0; i < star_count; ++i)
    randomize_star(&stars[i]);

  // set initial ship position
  x = current_col = cols / 2;
  z = 0.0f;
  speed = 0.0f;
  accel = accel_start;

  // generate starting track
  for (int col = 0; col < cols; ++col)
    track[0][col].live = track[0][col].correct = false;
  track[0][current_col].live = track[0][cols / 2].correct = true;
  randomize_colour(&track[0][current_col]);
  current_row = 1;
  for (int row = 0; row < rows - 1; ++row)
    new_row();
  current_row = 0;

}

// initialize
void init(void) {
  new_game();

  // lighting
  GLfloat mat_specular[] = { 1.0, 1.0, 1.0, 1.0 };
  GLfloat mat_shininess[] = { 50.0 };
  GLfloat light_position[] = { 1.0, 1.0, 1.0, 0.0 };
  glClearColor(0.0, 0.0, 0.0, 0.0);
  glShadeModel(GL_SMOOTH);
  glMaterialfv(GL_FRONT, GL_SPECULAR, mat_specular);
  glMaterialfv(GL_FRONT, GL_SHININESS, mat_shininess);
  glLightfv(GL_LIGHT0, GL_POSITION, light_position);
  glEnable(GL_LIGHTING);
  glEnable(GL_LIGHT0);
  glEnable(GL_NORMALIZE);
  glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
  glEnable(GL_COLOR_MATERIAL);
}

void space(void) {
  switch (state) {
    case Start:
      state = Flying;
      break;

    case Dead:
      new_game();
      break;

    default:
      break;
  }
}

void keypress(unsigned char key, int x, int y) {
  //printf("keypress %i\n", (int)key);

  switch (key) {

    // escape
    case 27:
      exit(0);
      break;

    // space bar
    case 32:
      space();
      break;

    default:
      break;
  }
}

void special_keypress(int key, int x, int y) {
  //printf("special keypress %i\n", key);

  switch (key) {

    // left arrow
    case GLUT_KEY_LEFT:
      if (state == Flying && current_col > 0)
        --current_col;
      break;

    // right arrow
    case GLUT_KEY_RIGHT:
      if (state == Flying && current_col < cols - 1)
        ++current_col;
      break;

    default:
      break;
  }
}

void advance() {

  // move forward
  z += speed * frame_time;
  if (z > 1.0f) {
    z -= 1.0f;
    new_row();
    //print_row(current_row);
  }

  // drift to column
  if (current_col > x) {
    x += (drift_linear + drift_mult * (current_col - x)) * frame_time;
    if (x > current_col)
      x = current_col;
  } else {
    x -= (drift_linear + drift_mult * (x - current_col)) * frame_time;
    if (x < current_col)
      x = current_col;
  }

  // move starfield forward (fake perspective, lots of magic numbers)
  for (int i = 0; i < star_count; ++i) {
    Star* star = &stars[i];
    star->x = (star->x - 0.5f) * (1.0f + speed * frame_time / 10.0f / star->distance) + 0.5f;
    star->y += speed * frame_time / 10.0f / star->distance;
    if (star->y > 1.0f) {
      star->y = 0.0f;
      star->x = rand_float_range(0.0f, 1.0f);
    }
  }
}

void explode(void) {
  //printf("BOOM! you are dead.\n");
  state = Exploding;
}

void tick_flight(void) {

  // accelerate and decay
  speed += accel * frame_time;
  accel = (accel - accel_min) * accel_mult + accel_min;

  // move forward
  advance();
  score += speed * frame_time * score_mult;

  // test for crash
  // note that we test one above current row; current row is really the back row, and the ship is on the next one
  if (!track[(current_row + 1) % rows][current_col].live)
    explode();

  // emit particles from engines
  Particle particle;
  particle.x = 0.17f - 1.0f * x + (cols - 1) / 2.0f;
  particle.y = 0.25f;
  particle.z = -0.5f;
  particle.x_speed = rand_float() * engine_xy_speed_max;
  particle.y_speed = rand_float() * engine_xy_speed_max;
  particle.z_speed = engine_z_speed;
  particle.size = engine_size;
  particle.fade = 1.0f;
  particle.red = particle.green = 10;
  particle.blue = 255;
  emit(&particle);
  particle.x = -0.17f - 1.0f * x + (cols - 1) / 2.0f;
  emit(&particle);
}

void tick_explode() {

  // move forward
  advance();

  // slow down
  speed = speed * decel_mult - decel_linear;
  if (speed < 0.0f)
    state = Dead;

  // emit particles from ship
  for (int i = 0; i < 6; ++i) {
    Particle particle;
    particle.x = rand_float() * 0.25f - 1.0f * x + (cols - 1) / 2.0f;
    particle.y = rand_float() * 0.1f + 0.25f;
    particle.z = rand_float() * 0.5f;
    particle.x_speed = rand_float() * explode_speed;
    particle.y_speed = rand_float() * explode_speed;
    particle.z_speed = rand_float() * explode_speed;
    particle.size = explode_size;
    particle.fade = 1.0f;
    particle.blue = 0;
    particle.green = 30;
    particle.red = 255;
    emit(&particle);
  }
}

void tick(void) {

  // calculate delta
  static int last_time = glutGet(GLUT_ELAPSED_TIME);
  int current_time = glutGet(GLUT_ELAPSED_TIME);
  static float delta_buffer = 0.0f;
  delta_buffer += (current_time - last_time) / 1000.0f;
  last_time = current_time;

  // safety catch in case game is paused for a while
  if (delta_buffer > frame_time * 100) {
    if (state == Flying)
      explode();
    delta_buffer = 0.0f;
    return;
  }

  // ticks
  for (; delta_buffer > frame_time; delta_buffer -= frame_time) {

    // propagate tick event to states
    switch (state) {
      case Start:
        break;

      case Flying:
        tick_flight();
        break;

      case Exploding:
        tick_explode();
        break;

      case Dead:
        break;

      default:
        break;
    }

    // tick particle system
    for (int i = 0; i < particle_count; ++i) {
      Particle* particle = &particles[(last_particle + particle_count - i) % particle_count];

      // as soon as we hit a dead particle, we can stop
      if (particle->fade < FLT_EPSILON)
        break;

      // move particles
      particle->x += particle->x_speed * frame_time;
      particle->y += particle->y_speed * frame_time;
      particle->z += particle->z_speed * frame_time;
      particle->fade -= fade_speed * frame_time;
    }
  }
}

void paint_string(const char* str, float x, float y) {  
  glRasterPos2f(x, y);
  for (; *str != '\0'; ++str)
    glutBitmapCharacter(GLUT_BITMAP_HELVETICA_18, *str);
}

void ortho(void) {
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glOrtho(0, w, h, 0, -FLT_MAX, FLT_MAX);
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();
}

void paint_track_segment(Segment* segment, int row, int col) {
  if (!segment->live)
    return;
  glPushMatrix();
    glTranslatef(- col + (cols - 1) / 2.0f, 0.0f, row-0.0f);
    glColor4f(segment->red / 255.0f, segment->green / 255.0f, segment->blue / 255.0f, 1.0f);
    glutSolidCube(1.0f);
  glPopMatrix();
}

void paint(void) {
  tick();
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  glEnable(GL_CULL_FACE);

  // prepare starfield
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_LIGHTING);
  glEnable(GL_POINT_SMOOTH);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glPointSize(2.0f);
  glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

  // paint starfield
  ortho();
  for (int i = 0; i < star_count; ++i) {
    Star* star = &stars[i];
    glPointSize(star->size);
    glBegin(GL_POINTS);
      glVertex2f(star->x * w, star->y * h);
    glEnd();
  }
  glDisable(GL_BLEND);

  // set perspective projection
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  gluPerspective(45, (float)w / (float)h, 1, 1000);
  
  // look ahead
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();
  const float dist = 3.5f;
  gluLookAt(-0.0f, dist, -dist, 
             0.0f, 0.0f,  dist,
             0.0f, 1.0f,  0.0f);

  // setup world
  glEnable(GL_LIGHTING);

  // transform world
  glPushMatrix();
    glScalef(segment_width, segment_height, segment_depth);
    glTranslatef(0.0f, 0.0f, -z);
    
    // zoom out
    //glScalef(0.1f, 0.1f, 0.1f);

    // paint track back to front and outside in, since we are without a depth buffer (apparently broken on some platforms)
    for (int row = rows - 1; row >= 0; --row) {
      paint_track_segment(&track[(row + current_row) % rows][0], row, 0);
      paint_track_segment(&track[(row + current_row) % rows][4], row, 4);
      paint_track_segment(&track[(row + current_row) % rows][1], row, 1);
      paint_track_segment(&track[(row + current_row) % rows][3], row, 3);
      paint_track_segment(&track[(row + current_row) % rows][2], row, 2);
    }
  glPopMatrix();

  // paint ship (lots of magic numbers!)
  glEnable(GL_DEPTH_TEST);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  float alpha = (state == Dead ? 0.0f : (state == Exploding ? (speed / explode_fade_speed) : 1.0f));
  if (alpha > 1.0f)
    alpha = 1.0f;
  else if (alpha < 0.0f)
    alpha = 0.0f;
  glPushMatrix();
    glTranslatef(- 1.0f * x + (cols - 1) / 2.0f, 0.25f, 0.0f);
    glScalef(0.25f, 0.15f, 0.5f);
    glColor4f(1.0f, 1.0f, 1.0f, alpha);
    glutSolidSphere(1.0f, 12, 12);
    glPushMatrix();
      glColor4f(0.8f, 0.8f, 1.0f, alpha);
      glScalef(0.5f, 1.20f, 0.65f);
      glTranslatef(0.0f, 0.0f, -0.8f);
      glTranslatef(-1.4f, 0.0f, 0.0f);
      glutSolidSphere(1.0f, 12, 12);
      glTranslatef(2.8f, 0.0f, 0.0f);
      glutSolidSphere(1.0f, 12, 12);
    glPopMatrix();
  glPopMatrix();

  // paint particles (flat additive blending)
  glDisable(GL_LIGHTING);
  glDisable(GL_DEPTH_TEST);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE);
  for (int i = 0; i < particle_count; ++i) {
    Particle* particle = &particles[(last_particle + particle_count - i) % particle_count];

    // as soon as we hit a dead particle, we can stop
    if (particle->fade < FLT_EPSILON)
      break;

    // paint it
    glColor4f(particle->red / 255.0f, particle->green / 255.0f, particle->blue / 255.0f, particle->fade);
    glPushMatrix();
      glTranslatef(particle->x, particle->y, particle->z);
      float size = particle->size * (2.0f - particle->fade);
      glScalef(size, size, size);
      glutSolidSphere(1.0f, 8, 8);
    glPopMatrix();
  }
  glDisable(GL_BLEND);

  // setup painting UI (ortho projection)
  ortho();
  glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
  float fonth = 18.0f;
  float pad = 4.0f;
  float y = fonth + pad / 2;

  // paint score
  char buf[50];
  snprintf(buf, sizeof(buf), "Score: %i", (int)roundf(score));
  paint_string(buf, pad, y);
  y += fonth + pad;

  // paint instructions
  if (state == Start) {
    paint_string("Press SPACEBAR to start.", pad, y);
    y += fonth + pad;
    paint_string("Press LEFT or RIGHT to move.", pad, y);
  } else if (state == Exploding || state == Dead) {
    paint_string("You are dead!", pad, y);
    y += fonth + pad;
    if (state == Dead) {
      paint_string("Press SPACEBAR to start over.", pad, y);
    }
  }
  
  // flip
  glutSwapBuffers();
}

void resize(int neww, int newh) {
  if (newh == 0)
    newh = 1;
  w = neww;
  h = newh;
  glViewport(0, 0, w, h);
}

int main(int argc, char** argv) {
  srand(time(0));

  // start up glut
  glutInit(&argc, argv);
  glutInitDisplayMode(GLUT_DEPTH | GLUT_DOUBLE | GLUT_RGBA);
  glutInitWindowPosition(100, 100);
  glutInitWindowSize(640, 480);
  glutCreateWindow("Space Run");

  // our callbacks
  glutDisplayFunc(paint);
  glutIdleFunc(paint);
  glutReshapeFunc(resize);
  glutKeyboardFunc(keypress);
  glutSpecialFunc(special_keypress);

  // go
  init();
  glutMainLoop();

  return 0;
}

#if defined(_WIN32) && !defined(CONSOLE)
INT APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPTSTR lpCmdLine, INT nCmdShow) {
  // i don't know whether this chunk of code works, nor do i particularly care

  // copy/decode command line
  #ifdef UNICODE
  size_t cmdlinecount = wcstombs(0, lpCmdLine, 0) + 1;
  char* cmdline = new char[cmdlinecount];
  wcstombs(cmdline, lpCmdLine, cmdlinecount);
  #else
  char* cmdline = new char[strlen(lpCmdLine) + 1];
  memcpy(cmdline, lpCmdLine, strlen(cmdline) + 1);
  #endif

  // tokenize to argv
  #define MAX_ARGS 10
  int argc = 0;
  char* argv[MAX_ARGS];
  char* token = strtok(cmdline, " ");
  while (token && argc < MAX_ARGS) {
    if (strlen(token) == 0)
      continue;
    argv[argc++] = token;
    token = strtok(0, " ");
  }

  // run
  int ret = main(argc, argv);

  // cleanup
  delete[] cmdline;
  return ret;
}
#endif


