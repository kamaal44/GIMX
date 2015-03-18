/*
 Copyright (c) 2014 Mathieu Laurendeau
 License: GPLv3
 */

#include <connectors/usb_con.h>
#include <connectors/protocol.h>
#include <adapter.h>
#include <mainloop.h>
#include <report2event/ds42event.h>
#include <report2event/360Pad2event.h>
#include <report2event/xOnePad2event.h>
#include <gimx.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <libusb-1.0/libusb.h>

#if !defined(LIBUSB_API_VERSION) && !defined(LIBUSBX_API_VERSION)
const char * LIBUSB_CALL libusb_strerror(enum libusb_error errcode)
{
  return libusb_error_name(errcode);
}
#endif

static libusb_context* ctx = NULL;
static libusb_device** devs = NULL;
static ssize_t cnt = 0;
static int nb_opened = 0;

#define REPORTS_MAX 2

static struct
{
  const char* name;
  unsigned short vendor;
  unsigned short product;
  struct
  {
    struct
    {
      unsigned char address;
      unsigned short size;
      struct
      {
        unsigned char nb;
        struct
        {
          unsigned char report_id;
          unsigned char report_length;
          void (*r2e)(int adapter_id, s_report* current, s_report* previous,
                int joystick_id, int (*callback)(GE_Event*));
        } elements[REPORTS_MAX];
      } reports;
    } in;
    struct
    {
      unsigned char address;
      unsigned short size;
    } out;
  } endpoints;
} controller[C_TYPE_MAX] =
{
  [C_TYPE_DS4] =
  {
    .name = DS4_DEVICE_NAME,
    .vendor = DS4_VENDOR,
    .product = DS4_PRODUCT,
    .endpoints =
    {
      .in =
      {
        .address = DS4_USB_INTERRUPT_ENDPOINT_IN | LIBUSB_ENDPOINT_IN,
        .size = DS4_USB_INTERRUPT_PACKET_SIZE,
        .reports =
        {
          .nb = 1,
          .elements =
          {
            {
              .report_id = DS4_USB_HID_IN_REPORT_ID,
              .report_length = DS4_USB_INTERRUPT_PACKET_SIZE,
              .r2e = ds42event
            }
          }
        }
      },
      .out =
      {
        .address = DS4_USB_INTERRUPT_ENDPOINT_OUT | LIBUSB_ENDPOINT_OUT,
        .size = DS4_USB_INTERRUPT_PACKET_SIZE
      }
    }
  },
  [C_TYPE_T300RS_PS4] =
  {
    .name = DS4_DEVICE_NAME,
    .vendor = DS4_VENDOR,
    .product = DS4_PRODUCT,
    .endpoints =
    {
      .in =
      {
        .address = DS4_USB_INTERRUPT_ENDPOINT_IN | LIBUSB_ENDPOINT_IN,
        .size = DS4_USB_INTERRUPT_PACKET_SIZE,
        .reports =
        {
          .nb = 1,
          .elements =
          {
            {
              .report_id = DS4_USB_HID_IN_REPORT_ID,
              .report_length = DS4_USB_INTERRUPT_PACKET_SIZE,
              .r2e = ds42event
            }
          }
        }
      },
      .out =
      {
        .address = DS4_USB_INTERRUPT_ENDPOINT_OUT | LIBUSB_ENDPOINT_OUT,
        .size = DS4_USB_INTERRUPT_PACKET_SIZE
      }
    }
  },
  [C_TYPE_360_PAD] =
  {
    .name = X360_NAME,
    .vendor = X360_VENDOR,
    .product = X360_PRODUCT,
    .endpoints =
    {
      .in =
      {
        .address = X360_USB_INTERRUPT_ENDPOINT_IN | LIBUSB_ENDPOINT_IN,
        .size = X360_USB_INTERRUPT_PACKET_SIZE,
        .reports =
        {
          .nb = 1,
          .elements =
          {
            {
              .report_id = X360_USB_HID_IN_REPORT_ID,
              .report_length = sizeof(s_report_x360),
              .r2e = _360Pad2event
            }
          }
        }
      },
      .out =
      {
        .address = X360_USB_INTERRUPT_ENDPOINT_OUT | LIBUSB_ENDPOINT_OUT,
        .size = X360_USB_INTERRUPT_PACKET_SIZE
      }
    }
  },
  [C_TYPE_XONE_PAD] =
  {
    .name = XONE_NAME,
    .vendor = XONE_VENDOR,
    .product = XONE_PRODUCT,
    .endpoints =
    {
      .in =
      {
        .address = XONE_USB_INTERRUPT_ENDPOINT_IN | LIBUSB_ENDPOINT_IN,
        .size = XONE_USB_INTERRUPT_PACKET_SIZE,
        .reports =
        {
          .nb = 2,
          .elements =
          {
            {
              .report_id = XONE_USB_HID_IN_REPORT_ID,
              .report_length = sizeof(((s_report_xone*)NULL)->input),
              .r2e = xOnePad2event
            },
            {
              .report_id = XONE_USB_HID_IN_GUIDE_REPORT_ID,
              .report_length = sizeof(((s_report_xone*)NULL)->guide),
              .r2e = xOnePad2event
            },
          }
        }
      },
      .out =
      {
        .address = XONE_USB_INTERRUPT_ENDPOINT_OUT | LIBUSB_ENDPOINT_OUT,
        .size = XONE_USB_INTERRUPT_PACKET_SIZE
      }
    }
  }
};

static int usb_state_indexes[MAX_CONTROLLERS] = {};

static struct usb_state {
  e_controller_type type;
  libusb_device_handle* devh;
  unsigned char ack;
  int joystick_id;
  s_report_packet report;
} usb_states[MAX_CONTROLLERS];

int (*event_callback)(GE_Event*) = NULL;

void usb_con_set_callback(int (*fp)(GE_Event*))
{
  event_callback = fp;
}

static int usb_poll_interrupt(int usb_number);

static void process_report(int usb_number, struct usb_state * state, struct libusb_transfer * transfer)
{
  int i;
  for(i = 0; i < controller[state->type].endpoints.in.reports.nb; ++i)
  {
    unsigned char report_id = controller[state->type].endpoints.in.reports.elements[i].report_id;
    unsigned char report_length = controller[state->type].endpoints.in.reports.elements[i].report_length;
    if(transfer->buffer[0] == report_id)
    {
      if(transfer->actual_length == report_length)
      {
        s_report* current = (s_report*) transfer->buffer;
        s_report* previous = &state->report.value;

        controller[state->type].endpoints.in.reports.elements[i].r2e(usb_number,
            (s_report*)current, (s_report*)previous, state->joystick_id, event_callback);

        if(state->type == C_TYPE_DS4 || state->type == C_TYPE_T300RS_PS4)
        {
          state->report.value.ds4 = current->ds4;
        }
        else if(state->type == C_TYPE_360_PAD)
        {
          state->report.value.x360 = current->x360;
        }
      }
      else
      {
        fprintf(stderr, "incorrect report length on interrupt endpoint: received %d bytes, expected %d bytes\n", transfer->actual_length, report_length);
      }
      break;
    }
  }
}

void usb_callback(struct libusb_transfer* transfer)
{
  int usb_number = *(int*)transfer->user_data;
  struct usb_state * state = usb_states+usb_number;

  struct libusb_control_setup* setup = libusb_control_transfer_get_setup(transfer);

  if(transfer->type == LIBUSB_TRANSFER_TYPE_CONTROL)
  {
    if(transfer->status == LIBUSB_TRANSFER_COMPLETED)
    {
      if(setup->bmRequestType & LIBUSB_ENDPOINT_IN)
      {
        if(transfer->actual_length > 0xff)
        {
          fprintf(stderr, "wLength (%hu) is higher than %hu\n", transfer->actual_length, BUFFER_SIZE-LIBUSB_CONTROL_SETUP_SIZE);
        }
        else
        {
          unsigned char *data = libusb_control_transfer_get_data(transfer);
          if(adapter_forward_control_in(usb_number, data, transfer->actual_length) < 0)
          {
            fprintf(stderr, "can't forward data to the adapter\n");
          }
        }
      }
    }
    else
    {
      fprintf(stderr, "libusb_transfer failed with status %d (bmRequestType=0x%02x, bRequest=0x%02x, wValue=0x%04x)\n", transfer->status, setup->bmRequestType, setup->bRequest, setup->wValue);
    }
  }
  else if(transfer->type == LIBUSB_TRANSFER_TYPE_INTERRUPT)
  {
    if(transfer->endpoint == controller[state->type].endpoints.in.address)
    {
      state->ack = 1;
    }

    if(transfer->status == LIBUSB_TRANSFER_COMPLETED)
    {
      if(transfer->endpoint == controller[state->type].endpoints.in.address)
      {
        // process joystick events
        if(transfer->actual_length <= controller[state->type].endpoints.in.size
            && transfer->actual_length > 0)
        {
          process_report(usb_number, state, transfer);
        }
      }
    }
    else
    {
      if(transfer->status != LIBUSB_TRANSFER_TIMED_OUT)
      {
        fprintf(stderr, "libusb_transfer failed with status %d (endpoint=0x%02x)\n", transfer->status, transfer->endpoint);
      }
    }
  }
}

static int usb_poll_interrupt(int usb_number)
{
  struct usb_state* state = usb_states+usb_number;
  struct libusb_transfer* transfer = libusb_alloc_transfer(0);
  transfer->flags = LIBUSB_TRANSFER_FREE_BUFFER | LIBUSB_TRANSFER_FREE_TRANSFER ;
  unsigned int size = controller[state->type].endpoints.in.size;
  unsigned char* buf = calloc(size, sizeof(char));
  libusb_fill_interrupt_transfer(transfer, state->devh, controller[state->type].endpoints.in.address, buf, size, (libusb_transfer_cb_fn)usb_callback, usb_state_indexes+usb_number, 1000);
  int ret = libusb_submit_transfer(transfer);
  if(ret < 0)
  {
    fprintf(stderr, "libusb_submit_transfer: %s.\n", libusb_strerror(ret));
    free(transfer->buffer);
    libusb_free_transfer(transfer);
  }
  else
  {
    state->ack = 0;
  }
  return ret;
}

void usb_poll_interrupts()
{
  int i;
  for(i=0; i<MAX_CONTROLLERS; ++i)
  {
    if(usb_states[i].devh && usb_states[i].ack)
    {
      usb_poll_interrupt(i);
    }
  }
}

int usb_handle_events(int unused)
{
#ifndef WIN32
  return libusb_handle_events(ctx);
#else
  if(ctx != NULL)
  {
    struct timeval tv = {};
    return libusb_handle_events_timeout(ctx, &tv);
  }
  else
  {
    return 0;
  }
#endif
}

int usb_init(int usb_number, e_controller_type type)
{
  int ret = -1;
  int dev_i;

  struct usb_state* state = usb_states+usb_number;

  if(!controller[type].vendor || !controller[type].product)
  {
    printf(_("no pass-through device is needed\n"));
    return 0;
  }

  usb_state_indexes[usb_number] = usb_number;

  memset(state, 0x00, sizeof(*state));
  state->joystick_id = -1;
  state->type = type;

  if(!ctx)
  {
    ret = libusb_init(&ctx);
    if(ret < 0)
    {
      fprintf(stderr, "libusb_init: %s.\n", libusb_strerror(ret));
      return -1;
    }
  }

  if(!devs)
  {
    cnt = libusb_get_device_list(ctx, &devs);
    if(cnt < 0)
    {
      fprintf(stderr, "libusb_get_device_list: %s.\n", libusb_strerror(cnt));
      return -1;
    }
  }

  for(dev_i=0; dev_i<cnt; ++dev_i)
  {
    struct libusb_device_descriptor desc;
    ret = libusb_get_device_descriptor(devs[dev_i], &desc);
    if(!ret)
    {
      if(desc.idVendor == controller[type].vendor && desc.idProduct == controller[type].product)
      {
        libusb_device_handle* devh;
        ret = libusb_open(devs[dev_i], &devh);
        if(ret < 0)
        {
          fprintf(stderr, "libusb_open: %s.\n", libusb_strerror(ret));
          return -1;
        }
        else
        {
          ret = libusb_reset_device(devh);
          if(ret != LIBUSB_SUCCESS)
          {
            fprintf(stderr, "libusb_detach_kernel_driver: %s.\n", libusb_strerror(ret));
            libusb_close(devh);
            return -1;
          }

#if defined(LIBUSB_API_VERSION) || defined(LIBUSBX_API_VERSION)
          libusb_set_auto_detach_kernel_driver(devh, 1);
#else
#ifndef WIN32
          if(libusb_kernel_driver_active(devh, 0))
          {
            ret = libusb_detach_kernel_driver(devh, 0);
            if(ret < 0)
            {
              fprintf(stderr, "libusb_detach_kernel_driver: %s.\n", libusb_strerror(ret));
              libusb_close(devh);
              return -1;
            }
          }
#endif
#endif

          ret = libusb_claim_interface(devh, 0);
          if(ret < 0)
          {
            fprintf(stderr, "libusb_claim_interface: %s.\n", libusb_strerror(ret));
            libusb_close(devh);
          }
          else
          {
            state->devh = devh;
            ++nb_opened;

#ifndef WIN32
            const struct libusb_pollfd** pfd_usb = libusb_get_pollfds(ctx);

            int poll_i;
            for (poll_i=0; pfd_usb[poll_i] != NULL; ++poll_i)
            {
              GE_AddSource(pfd_usb[poll_i]->fd, usb_number, usb_handle_events, usb_handle_events, usb_close);
            }

            free(pfd_usb);
#endif
            if(usb_poll_interrupt(usb_number) == LIBUSB_SUCCESS)
            {
              // register joystick
              int device_id = GE_RegisterJoystick(controller[state->type].name, NULL);
              if(device_id >= 0)
              {
                state->joystick_id = device_id;
              }
            }

            return 0;
          }
        }
      }
    }
  }

  return -1;
}

int usb_close(int usb_number)
{
  struct usb_state* state = usb_states+usb_number;

  if(state->devh)
  {
    //TODO: cancel and free pending transfers
    libusb_release_interface(state->devh, 0);
#if !defined(LIBUSB_API_VERSION) && !defined(LIBUSBX_API_VERSION)
#ifndef WIN32
    libusb_attach_kernel_driver(state->devh, 0);
#endif
#endif
    libusb_close(state->devh);
    state->devh = NULL;
    --nb_opened;
    if(!nb_opened)
    {
      libusb_free_device_list(devs, 1);
      devs = NULL;
      libusb_exit(ctx);
      ctx = NULL;
    }
  }

  set_done();

  return 1;
}

int usb_send_control(int usb_number, unsigned char* buffer, unsigned char length)
{
  struct usb_state* state = usb_states+usb_number;

  if(!state->devh)
  {
    fprintf(stderr, "no usb device opened for index %d\n", usb_number);
    return -1;
  }

  struct libusb_control_setup* control_setup = (struct libusb_control_setup*)buffer;
  if(control_setup->wLength > BUFFER_SIZE-LIBUSB_CONTROL_SETUP_SIZE)
  {
    fprintf(stderr, "wLength (%hu) is higher than %hu\n", control_setup->wLength, BUFFER_SIZE-LIBUSB_CONTROL_SETUP_SIZE);
    return -1;
  }

  unsigned int size = length;

  if(control_setup->bmRequestType & LIBUSB_ENDPOINT_IN)
  {
    size += control_setup->wLength;
  }

  unsigned char* buf = calloc(size, sizeof(char));
  if(!buf)
  {
    fprintf(stderr, "calloc failed\n");
    return -1;
  }

  memcpy(buf, buffer, length);

  struct libusb_transfer* transfer = libusb_alloc_transfer(0);

  transfer->flags = LIBUSB_TRANSFER_FREE_BUFFER | LIBUSB_TRANSFER_FREE_TRANSFER;

  libusb_fill_control_transfer(transfer, state->devh, buf, (libusb_transfer_cb_fn)usb_callback, usb_state_indexes+usb_number, 1000);

  int ret = libusb_submit_transfer(transfer);
  if(ret < 0)
  {
    fprintf(stderr, "libusb_submit_transfer: %s.\n", libusb_strerror(ret));
    free(transfer->buffer);
    libusb_free_transfer(transfer);
    return -1;
  }

  return 0;
}

int usb_send_interrupt_out(int usb_number, unsigned char* buffer, unsigned char length)
{
  struct usb_state* state = usb_states+usb_number;

  if(!state->devh)
  {
    fprintf(stderr, "no usb device opened for index %d\n", usb_number);
    return -1;
  }

  unsigned char* buf = calloc(length, sizeof(unsigned char));
  if(!buf)
  {
    fprintf(stderr, "calloc failed\n");
    return -1;
  }

  memcpy(buf, buffer, length);

  struct libusb_transfer* transfer = libusb_alloc_transfer(0);

  transfer->flags = LIBUSB_TRANSFER_FREE_BUFFER | LIBUSB_TRANSFER_FREE_TRANSFER;

  libusb_fill_interrupt_transfer(transfer, state->devh, controller[state->type].endpoints.out.address, buf, length, (libusb_transfer_cb_fn)usb_callback, usb_state_indexes+usb_number, 1000);

  int ret = libusb_submit_transfer(transfer);
  if(ret < 0)
  {
    fprintf(stderr, "libusb_submit_transfer: %s.\n", libusb_strerror(ret));
    free(transfer->buffer);
    libusb_free_transfer(transfer);
    return -1;
  }

  return 0;
}
