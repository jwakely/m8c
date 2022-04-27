// Copyright 2021 Jonne Kokkonen
// Released under the MIT licence, https://opensource.org/licenses/MIT

#include <SDL.h>
#include <SDL2/SDL_timer.h>
#include <libserialport.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "command.h"
#include "config.h"
#include "fx_cube.h"
#include "input.h"
#include "render.h"
#include "serial.h"
#include "slip.h"
#include "write.h"

// maximum amount of bytes to read from the serial in one read()
#define serial_read_size 324

int8_t run = 2;
uint8_t need_display_reset = 0;

// Handles CTRL+C / SIGINT
void intHandler(int dummy) { run = -1; }

int main(int argc, char *argv[]) {

  // Initialize the config to defaults read in the params from the
  // configfile if present
  config_params_s conf = init_config();

  // TODO: take cli parameter to override default configfile location
  read_config(&conf);

  // allocate memory for serial buffer
  uint8_t *serial_buf = malloc(serial_read_size);

  static uint8_t slip_buffer[serial_read_size]; // SLIP command buffer

  // settings for the slip packet handler
  static const slip_descriptor_s slip_descriptor = {
      .buf = slip_buffer,
      .buf_size = sizeof(slip_buffer),
      .recv_message = process_command, // the function where complete slip
                                       // packets are processed further
  };

  static slip_handler_s slip;

  signal(SIGINT, intHandler);
  signal(SIGTERM, intHandler);

  slip_init(&slip, &slip_descriptor);

  if (initialize_sdl(conf.init_fullscreen, conf.init_use_gpu) == -1)
    run = -1;

kiosk:

  screensaver_init();

  struct sp_port *port;

  while (run == 2) {
    static uint32_t ticks = 0;
    static uint32_t ticks2 = 0;
    if (SDL_GetTicks() - ticks > 500) {
      ticks = SDL_GetTicks();
      if (check_serial_connection())
        run = 1;
    }
    if (SDL_GetTicks() - ticks2 > 16) {
      ticks2 = SDL_GetTicks();
      screensaver_draw();
      render_screen();
    }
    input_msg_s input = get_input_msg(&conf);
    if (input.value == msg_quit) {
      run = -1;
    }
    SDL_Delay(1);
  }

  screensaver_destroy();

  port = init_serial();
  if (port == NULL)
    run = -1;

  if (enable_and_reset_display(port) == -1)
    run = -1;

  uint8_t prev_input = 0;

  // main loop
  while (run == 1) {
    static int ticks = 0;
    if (SDL_GetTicks() - ticks > 500) {
      ticks = SDL_GetTicks();
      if (!check_serial_connection()) {
        run = 0;
        SDL_Log("Device disconnected");
      }
    }
    // get current inputs
    input_msg_s input = get_input_msg(&conf);

    switch (input.type) {
    case normal:
      if (input.value != prev_input) {
        prev_input = input.value;
        send_msg_controller(port, input.value);
      }
      break;
    case keyjazz:
      if (input.value != prev_input) {
        prev_input = input.value;
        if (input.value != 0) {
          send_msg_keyjazz(port, input.value, 0xFF);
        } else {
          send_msg_keyjazz(port, 0, 0);
        }
      }
      break;
    case special:
      if (input.value != prev_input) {
        prev_input = input.value;
        switch (input.value) {
        case msg_quit:
          run = -1;
          break;
        case msg_reset_display:
          reset_display(port);
          break;
        default:
          break;
        }
        break;
      }
    }

    // read serial port
    int bytes_read = sp_blocking_read(port, serial_buf, serial_read_size, 3);
    if (bytes_read < 0) {
      SDL_LogCritical(SDL_LOG_CATEGORY_ERROR, "Error %d reading serial. \n",
                      (int)bytes_read);
      run = -1;
    }
    if (bytes_read > 0) {
      for (int i = 0; i < bytes_read; i++) {
        uint8_t rx = serial_buf[i];
        // process the incoming bytes into commands and draw them
        int n = slip_read_byte(&slip, rx);
        if (n != SLIP_NO_ERROR) {
          if (n == SLIP_ERROR_INVALID_PACKET) {
            reset_display(port);
          } else {
            SDL_LogError(SDL_LOG_CATEGORY_ERROR, "SLIP error %d\n", n);
          }
        }
      }
      render_screen();
    }
  }

  // exit, clean up

  disconnect(port);
  sp_close(port);
  sp_free_port(port);
  if (run == -1) {
    SDL_Log("Shutting down\n");
    close_game_controllers();
    close_renderer();    
    free(serial_buf);
    return 0;
  } else {
    run = 2;
    goto kiosk;
  }
}
