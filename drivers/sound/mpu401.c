/*
 * sound/mpu401.c
 *
 * The low level driver for Roland MPU-401 compatible Midi cards.
 */
/*
 * Copyright (C) by Hannu Savolainen 1993-1996
 *
 * USS/Lite for Linux is distributed under the GNU GENERAL PUBLIC LICENSE (GPL)
 * Version 2 (June 1991). See the "COPYING" file distributed with this software
 * for more info.
 */
#include <linux/config.h>


#define USE_SEQ_MACROS
#define USE_SIMPLE_MACROS

#include "sound_config.h"

#if (defined(CONFIG_MPU401) || defined(CONFIG_MPU_EMU)) && defined(CONFIG_MIDI)
#include "coproc.h"

static int      init_sequence[20];	/* NOTE! pos 0 = len, start pos 1. */

#ifdef CONFIG_SEQUENCER
static int      timer_mode = TMR_INTERNAL, timer_caps = TMR_INTERNAL;

#endif

struct mpu_config
  {
    int             base;	/*
				 * I/O base
				 */
    int             irq;
    int             opened;	/*
				 * Open mode
				 */
    int             devno;
    int             synthno;
    int             uart_mode;
    int             initialized;
    int             mode;
#define MODE_MIDI	1
#define MODE_SYNTH	2
    unsigned char   version, revision;
    unsigned int    capabilities;
#define MPU_CAP_INTLG	0x10000000
#define MPU_CAP_SYNC	0x00000010
#define MPU_CAP_FSK	0x00000020
#define MPU_CAP_CLS	0x00000040
#define MPU_CAP_SMPTE 	0x00000080
#define MPU_CAP_2PORT	0x00000001
    int             timer_flag;

#define MBUF_MAX	10
#define BUFTEST(dc) if (dc->m_ptr >= MBUF_MAX || dc->m_ptr < 0) \
	{printk("MPU: Invalid buffer pointer %d/%d, s=%d\n", dc->m_ptr, dc->m_left, dc->m_state);dc->m_ptr--;}
    int             m_busy;
    unsigned char   m_buf[MBUF_MAX];
    int             m_ptr;
    int             m_state;
    int             m_left;
    unsigned char   last_status;
    void            (*inputintr) (int dev, unsigned char data);
    int             shared_irq;
    int            *osp;
  };

#define	DATAPORT(base)   (base)
#define	COMDPORT(base)   (base+1)
#define	STATPORT(base)   (base+1)

static int 
mpu401_status (struct mpu_config *devc)
{
  return inb (STATPORT (devc->base));
}
#define input_avail(devc)		(!(mpu401_status(devc)&INPUT_AVAIL))
#define output_ready(devc)		(!(mpu401_status(devc)&OUTPUT_READY))
static void 
write_command (struct mpu_config *devc, unsigned char cmd)
{
  outb (cmd, COMDPORT (devc->base));
}
static int 
read_data (struct mpu_config *devc)
{
  return inb (DATAPORT (devc->base));
}

static void 
write_data (struct mpu_config *devc, unsigned char byte)
{
  outb (byte, DATAPORT (devc->base));
}

#define	OUTPUT_READY	0x40
#define	INPUT_AVAIL	0x80
#define	MPU_ACK		0xFE
#define	MPU_RESET	0xFF
#define	UART_MODE_ON	0x3F

static struct mpu_config dev_conf[MAX_MIDI_DEV] =
{
  {0}};

static int      n_mpu_devs = 0;
static volatile int irq2dev[17] =
{-1, -1, -1, -1, -1, -1, -1, -1,
 -1, -1, -1, -1, -1, -1, -1, -1, -1};

static int      reset_mpu401 (struct mpu_config *devc);
static void     set_uart_mode (int dev, struct mpu_config *devc, int arg);

static void     mpu_timer_init (int midi_dev);
static void     mpu_timer_interrupt (void);
static void     timer_ext_event (struct mpu_config *devc, int event, int parm);

static struct synth_info mpu_synth_info_proto =
{"MPU-401 MIDI interface", 0, SYNTH_TYPE_MIDI, MIDI_TYPE_MPU401, 0, 128, 0, 128, SYNTH_CAP_INPUT};

static struct synth_info mpu_synth_info[MAX_MIDI_DEV];

/*
 * States for the input scanner
 */

#define ST_INIT			0	/* Ready for timing byte or msg */
#define ST_TIMED		1	/* Leading timing byte rcvd */
#define ST_DATABYTE		2	/* Waiting for (nr_left) data bytes */

#define ST_SYSMSG		100	/* System message (sysx etc). */
#define ST_SYSEX		101	/* System exclusive msg */
#define ST_MTC			102	/* Midi Time Code (MTC) qframe msg */
#define ST_SONGSEL		103	/* Song select */
#define ST_SONGPOS		104	/* Song position pointer */

static unsigned char len_tab[] =	/* # of data bytes following a status
					 */
{
  2,				/* 8x */
  2,				/* 9x */
  2,				/* Ax */
  2,				/* Bx */
  1,				/* Cx */
  1,				/* Dx */
  2,				/* Ex */
  0				/* Fx */
};

#ifndef CONFIG_SEQUENCER
#define STORE(cmd)
#else
#define STORE(cmd) \
{ \
  int len; \
  unsigned char obuf[8]; \
  cmd; \
  seq_input_event(obuf, len); \
}
#endif

#define _seqbuf obuf
#define _seqbufptr 0
#define _SEQ_ADVBUF(x) len=x

static int
mpu_input_scanner (struct mpu_config *devc, unsigned char midic)
{

  switch (devc->m_state)
    {
    case ST_INIT:
      switch (midic)
	{
	case 0xf8:
	  /* Timer overflow */
	  break;

	case 0xfc:
	  printk ("<all end>");
	  break;

	case 0xfd:
	  if (devc->timer_flag)
	    mpu_timer_interrupt ();
	  break;

	case 0xfe:
	  return MPU_ACK;
	  break;

	case 0xf0:
	case 0xf1:
	case 0xf2:
	case 0xf3:
	case 0xf4:
	case 0xf5:
	case 0xf6:
	case 0xf7:
	  printk ("<Trk data rq #%d>", midic & 0x0f);
	  break;

	case 0xf9:
	  printk ("<conductor rq>");
	  break;

	case 0xff:
	  devc->m_state = ST_SYSMSG;
	  break;

	default:
	  if (midic <= 0xef)
	    {
	      /* printk("mpu time: %d ", midic); */
	      devc->m_state = ST_TIMED;
	    }
	  else
	    printk ("<MPU: Unknown event %02x> ", midic);
	}
      break;

    case ST_TIMED:
      {
	int             msg = ((int) (midic & 0xf0) >> 4);

	devc->m_state = ST_DATABYTE;

	if (msg < 8)		/* Data byte */
	  {
	    /* printk("midi msg (running status) "); */
	    msg = ((int) (devc->last_status & 0xf0) >> 4);
	    msg -= 8;
	    devc->m_left = len_tab[msg] - 1;

	    devc->m_ptr = 2;
	    devc->m_buf[0] = devc->last_status;
	    devc->m_buf[1] = midic;

	    if (devc->m_left <= 0)
	      {
		devc->m_state = ST_INIT;
		do_midi_msg (devc->synthno, devc->m_buf, devc->m_ptr);
		devc->m_ptr = 0;
	      }
	  }
	else if (msg == 0xf)	/* MPU MARK */
	  {
	    devc->m_state = ST_INIT;

	    switch (midic)
	      {
	      case 0xf8:
		/* printk("NOP "); */
		break;

	      case 0xf9:
		/* printk("meas end "); */
		break;

	      case 0xfc:
		/* printk("data end "); */
		break;

	      default:
		printk ("Unknown MPU mark %02x\n", midic);
	      }
	  }
	else
	  {
	    devc->last_status = midic;
	    /* printk ("midi msg "); */
	    msg -= 8;
	    devc->m_left = len_tab[msg];

	    devc->m_ptr = 1;
	    devc->m_buf[0] = midic;

	    if (devc->m_left <= 0)
	      {
		devc->m_state = ST_INIT;
		do_midi_msg (devc->synthno, devc->m_buf, devc->m_ptr);
		devc->m_ptr = 0;
	      }
	  }
      }
      break;

    case ST_SYSMSG:
      switch (midic)
	{
	case 0xf0:
	  printk ("<SYX>");
	  devc->m_state = ST_SYSEX;
	  break;

	case 0xf1:
	  devc->m_state = ST_MTC;
	  break;

	case 0xf2:
	  devc->m_state = ST_SONGPOS;
	  devc->m_ptr = 0;
	  break;

	case 0xf3:
	  devc->m_state = ST_SONGSEL;
	  break;

	case 0xf6:
	  /* printk("tune_request\n"); */
	  devc->m_state = ST_INIT;

	  /*
	     *    Real time messages
	   */
	case 0xf8:
	  /* midi clock */
	  devc->m_state = ST_INIT;
	  timer_ext_event (devc, TMR_CLOCK, 0);
	  break;

	case 0xfA:
	  devc->m_state = ST_INIT;
	  timer_ext_event (devc, TMR_START, 0);
	  break;

	case 0xFB:
	  devc->m_state = ST_INIT;
	  timer_ext_event (devc, TMR_CONTINUE, 0);
	  break;

	case 0xFC:
	  devc->m_state = ST_INIT;
	  timer_ext_event (devc, TMR_STOP, 0);
	  break;

	case 0xFE:
	  /* active sensing */
	  devc->m_state = ST_INIT;
	  break;

	case 0xff:
	  /* printk("midi hard reset"); */
	  devc->m_state = ST_INIT;
	  break;

	default:
	  printk ("unknown MIDI sysmsg %0x\n", midic);
	  devc->m_state = ST_INIT;
	}
      break;

    case ST_MTC:
      devc->m_state = ST_INIT;
      printk ("MTC frame %x02\n", midic);
      break;

    case ST_SYSEX:
      if (midic == 0xf7)
	{
	  printk ("<EOX>");
	  devc->m_state = ST_INIT;
	}
      else
	printk ("%02x ", midic);
      break;

    case ST_SONGPOS:
      BUFTEST (devc);
      devc->m_buf[devc->m_ptr++] = midic;
      if (devc->m_ptr == 2)
	{
	  devc->m_state = ST_INIT;
	  devc->m_ptr = 0;
	  timer_ext_event (devc, TMR_SPP,
			   ((devc->m_buf[1] & 0x7f) << 7) |
			   (devc->m_buf[0] & 0x7f));
	}
      break;

    case ST_DATABYTE:
      BUFTEST (devc);
      devc->m_buf[devc->m_ptr++] = midic;
      if ((--devc->m_left) <= 0)
	{
	  devc->m_state = ST_INIT;
	  do_midi_msg (devc->synthno, devc->m_buf, devc->m_ptr);
	  devc->m_ptr = 0;
	}
      break;

    default:
      printk ("Bad state %d ", devc->m_state);
      devc->m_state = ST_INIT;
    }

  return 1;
}

static void
mpu401_input_loop (struct mpu_config *devc)
{
  unsigned long   flags;
  int             busy;
  int             n;

  save_flags (flags);
  cli ();
  busy = devc->m_busy;
  devc->m_busy = 1;
  restore_flags (flags);

  if (busy)			/* Already inside the scanner */
    return;

  n = 50;

  while (input_avail (devc) && n-- > 0)
    {
      unsigned char   c = read_data (devc);

      if (devc->mode == MODE_SYNTH)
	{
	  mpu_input_scanner (devc, c);
	}
      else if (devc->opened & OPEN_READ && devc->inputintr != NULL)
	devc->inputintr (devc->devno, c);
    }

  devc->m_busy = 0;
}

void
mpuintr (int irq, void *dev_id, struct pt_regs *dummy)
{
  struct mpu_config *devc;
  int             dev;

  sti ();

/*
 * FreeBSD (and some others) pass unit number to the interrupt handler.
 * In this case we have to scan the table for first handler.
 */

  if (irq < 1 || irq > 15)
    {
      dev = -1;
    }
  else
    dev = irq2dev[irq];

  if (dev == -1)
    {
      int             origirq = irq;

      for (irq = 0; irq <= 16; irq++)
	if (irq2dev[irq] != -1)
	  break;
      if (irq > 15)
	{
	  printk ("MPU-401: Bogus interrupt #%d?\n", origirq);
	  return;
	}
      dev = irq2dev[irq];
      devc = &dev_conf[dev];
    }
  else
    devc = &dev_conf[dev];

  if (input_avail (devc))
    if (devc->base != 0 && (devc->opened & OPEN_READ || devc->mode == MODE_SYNTH))
      mpu401_input_loop (devc);
    else
      {
	/* Dummy read (just to acknowledge the interrupt) */
	read_data (devc);
      }

}

static int
mpu401_open (int dev, int mode,
	     void            (*input) (int dev, unsigned char data),
	     void            (*output) (int dev)
)
{
  int             err;
  struct mpu_config *devc;

  if (dev < 0 || dev >= num_midis)
    return -(ENXIO);

  devc = &dev_conf[dev];

  if (devc->opened)
    {
      printk ("MPU-401: Midi busy\n");
      return -(EBUSY);
    }

  /*
     *  Verify that the device is really running.
     *  Some devices (such as Ensoniq SoundScape don't
     *  work before the on board processor (OBP) is initialized
     *  by downloadin it's microcode.
   */

  if (!devc->initialized)
    {
      if (mpu401_status (devc) == 0xff)		/* Bus float */
	{
	  printk ("MPU-401: Device not initialized properly\n");
	  return -(EIO);
	}
      reset_mpu401 (devc);
    }

  irq2dev[devc->irq] = dev;

  if (midi_devs[dev]->coproc)
    if ((err = midi_devs[dev]->coproc->
	 open (midi_devs[dev]->coproc->devc, COPR_MIDI)) < 0)
      {
	printk ("MPU-401: Can't access coprocessor device\n");

	return err;
      }

  set_uart_mode (dev, devc, 1);
  devc->mode = MODE_MIDI;
  devc->synthno = 0;

  mpu401_input_loop (devc);

  devc->inputintr = input;
  devc->opened = mode;

  return 0;
}

static void
mpu401_close (int dev)
{
  struct mpu_config *devc;

  devc = &dev_conf[dev];

  if (devc->uart_mode)
    reset_mpu401 (devc);	/*
				 * This disables the UART mode
				 */
  devc->mode = 0;

  devc->inputintr = NULL;

  if (midi_devs[dev]->coproc)
    midi_devs[dev]->coproc->close (midi_devs[dev]->coproc->devc, COPR_MIDI);
  devc->opened = 0;
}

static int
mpu401_out (int dev, unsigned char midi_byte)
{
  int             timeout;
  unsigned long   flags;

  struct mpu_config *devc;

  devc = &dev_conf[dev];

  /*
   * Sometimes it takes about 30000 loops before the output becomes ready
   * (After reset). Normally it takes just about 10 loops.
   */

  for (timeout = 30000; timeout > 0 && !output_ready (devc); timeout--);

  save_flags (flags);
  cli ();
  if (!output_ready (devc))
    {
      printk ("MPU-401: Send data timeout\n");
      restore_flags (flags);
      return 0;
    }

  write_data (devc, midi_byte);
  restore_flags (flags);
  return 1;
}

static int
mpu401_command (int dev, mpu_command_rec * cmd)
{
  int             i, timeout, ok;
  int             ret = 0;
  unsigned long   flags;
  struct mpu_config *devc;

  devc = &dev_conf[dev];

  if (devc->uart_mode)		/*
				 * Not possible in UART mode
				 */
    {
      printk ("MPU-401 commands not possible in the UART mode\n");
      return -(EINVAL);
    }

  /*
   * Test for input since pending input seems to block the output.
   */
  if (input_avail (devc))
    mpu401_input_loop (devc);

  /*
   * Sometimes it takes about 50000 loops before the output becomes ready
   * (After reset). Normally it takes just about 10 loops.
   */

  timeout = 50000;
retry:
  if (timeout-- <= 0)
    {
      printk ("MPU-401: Command (0x%x) timeout\n", (int) cmd->cmd);
      return -(EIO);
    }

  save_flags (flags);
  cli ();

  if (!output_ready (devc))
    {
      restore_flags (flags);
      goto retry;
    }

  write_command (devc, cmd->cmd);

  ok = 0;
  for (timeout = 50000; timeout > 0 && !ok; timeout--)
    if (input_avail (devc))
      {
	if (devc->opened && devc->mode == MODE_SYNTH)
	  {
	    if (mpu_input_scanner (devc, read_data (devc)) == MPU_ACK)
	      ok = 1;
	  }
	else
	  {			/* Device is not currently open. Use simplier method */
	    if (read_data (devc) == MPU_ACK)
	      ok = 1;
	  }
      }

  if (!ok)
    {
      restore_flags (flags);
      /*       printk ("MPU: No ACK to command (0x%x)\n", (int) cmd->cmd); */
      return -(EIO);
    }

  if (cmd->nr_args)
    for (i = 0; i < cmd->nr_args; i++)
      {
	for (timeout = 3000; timeout > 0 && !output_ready (devc); timeout--);

	if (!mpu401_out (dev, cmd->data[i]))
	  {
	    restore_flags (flags);
	    printk ("MPU: Command (0x%x), parm send failed.\n", (int) cmd->cmd);
	    return -(EIO);
	  }
      }

  ret = 0;
  cmd->data[0] = 0;

  if (cmd->nr_returns)
    for (i = 0; i < cmd->nr_returns; i++)
      {
	ok = 0;
	for (timeout = 5000; timeout > 0 && !ok; timeout--)
	  if (input_avail (devc))
	    {
	      cmd->data[i] = read_data (devc);
	      ok = 1;
	    }

	if (!ok)
	  {
	    restore_flags (flags);
	    /* printk ("MPU: No response(%d) to command (0x%x)\n", i, (int) cmd->cmd);  */
	    return -(EIO);
	  }
      }

  restore_flags (flags);

  return ret;
}

static int
mpu_cmd (int dev, int cmd, int data)
{
  int             ret;

  static mpu_command_rec rec;

  rec.cmd = cmd & 0xff;
  rec.nr_args = ((cmd & 0xf0) == 0xE0);
  rec.nr_returns = ((cmd & 0xf0) == 0xA0);
  rec.data[0] = data & 0xff;

  if ((ret = mpu401_command (dev, &rec)) < 0)
    {
      return ret;
    }
  return (unsigned char) rec.data[0];
}

static int
mpu401_prefix_cmd (int dev, unsigned char status)
{
  struct mpu_config *devc = &dev_conf[dev];

  if (devc->uart_mode)
    return 1;

  if (status < 0xf0)
    {
      if (mpu_cmd (dev, 0xD0, 0) < 0)
	{
	  return 0;
	}

      return 1;
    }

  switch (status)
    {
    case 0xF0:
      if (mpu_cmd (dev, 0xDF, 0) < 0)
	{
	  return 0;
	}

      return 1;
      break;

    default:
      return 0;
    }

}

static int
mpu401_start_read (int dev)
{
  return 0;
}

static int
mpu401_end_read (int dev)
{
  return 0;
}

static int
mpu401_ioctl (int dev, unsigned cmd, caddr_t arg)
{
  struct mpu_config *devc;

  devc = &dev_conf[dev];

  switch (cmd)
    {
    case 1:
      memcpy_fromfs ((char *) init_sequence, &((char *) arg)[0], sizeof (init_sequence));
      return 0;
      break;

    case SNDCTL_MIDI_MPUMODE:
      if (!(devc->capabilities & MPU_CAP_INTLG))	/* No intelligent mode */
	{
	  printk ("MPU-401: Intelligent mode not supported by the HW\n");
	  return -(EINVAL);
	}
      set_uart_mode (dev, devc, !get_fs_long ((long *) arg));
      return 0;
      break;

    case SNDCTL_MIDI_MPUCMD:
      {
	int             ret;
	mpu_command_rec rec;

	memcpy_fromfs ((char *) &rec, &((char *) arg)[0], sizeof (rec));

	if ((ret = mpu401_command (dev, &rec)) < 0)
	  return ret;

	memcpy_tofs (&((char *) arg)[0], (char *) &rec, sizeof (rec));
	return 0;
      }
      break;

    default:
      return -(EINVAL);
    }
}

static void
mpu401_kick (int dev)
{
}

static int
mpu401_buffer_status (int dev)
{
  return 0;			/*
				 * No data in buffers
				 */
}

static int
mpu_synth_ioctl (int dev,
		 unsigned int cmd, caddr_t arg)
{
  int             midi_dev;
  struct mpu_config *devc;

  midi_dev = synth_devs[dev]->midi_dev;

  if (midi_dev < 0 || midi_dev > num_midis)
    return -(ENXIO);

  devc = &dev_conf[midi_dev];

  switch (cmd)
    {

    case SNDCTL_SYNTH_INFO:
      memcpy_tofs (&((char *) arg)[0], &mpu_synth_info[midi_dev], sizeof (struct synth_info));

      return 0;
      break;

    case SNDCTL_SYNTH_MEMAVL:
      return 0x7fffffff;
      break;

    default:
      return -(EINVAL);
    }
}

static int
mpu_synth_open (int dev, int mode)
{
  int             midi_dev, err;
  struct mpu_config *devc;

  midi_dev = synth_devs[dev]->midi_dev;

  if (midi_dev < 0 || midi_dev > num_midis)
    {
      return -(ENXIO);
    }

  devc = &dev_conf[midi_dev];

  /*
     *  Verify that the device is really running.
     *  Some devices (such as Ensoniq SoundScape don't
     *  work before the on board processor (OBP) is initialized
     *  by downloadin it's microcode.
   */

  if (!devc->initialized)
    {
      if (mpu401_status (devc) == 0xff)		/* Bus float */
	{
	  printk ("MPU-401: Device not initialized properly\n");
	  return -(EIO);
	}
      reset_mpu401 (devc);
    }

  if (devc->opened)
    {
      printk ("MPU-401: Midi busy\n");
      return -(EBUSY);
    }

  devc->mode = MODE_SYNTH;
  devc->synthno = dev;

  devc->inputintr = NULL;
  irq2dev[devc->irq] = midi_dev;

  if (midi_devs[midi_dev]->coproc)
    if ((err = midi_devs[midi_dev]->coproc->
	 open (midi_devs[midi_dev]->coproc->devc, COPR_MIDI)) < 0)
      {
	printk ("MPU-401: Can't access coprocessor device\n");

	return err;
      }

  devc->opened = mode;
  reset_mpu401 (devc);

  if (mode & OPEN_READ)
    {
      mpu_cmd (midi_dev, 0x8B, 0);	/* Enable data in stop mode */
      mpu_cmd (midi_dev, 0x34, 0);	/* Return timing bytes in stop mode */
      mpu_cmd (midi_dev, 0x87, 0);	/* Enable pitch & controller */
    }

  return 0;
}

static void
mpu_synth_close (int dev)
{
  int             midi_dev;
  struct mpu_config *devc;

  midi_dev = synth_devs[dev]->midi_dev;

  devc = &dev_conf[midi_dev];
  mpu_cmd (midi_dev, 0x15, 0);	/* Stop recording, playback and MIDI */
  mpu_cmd (midi_dev, 0x8a, 0);	/* Disable data in stopped mode */

  devc->inputintr = NULL;

  if (midi_devs[midi_dev]->coproc)
    midi_devs[midi_dev]->coproc->close (midi_devs[midi_dev]->coproc->devc, COPR_MIDI);
  devc->opened = 0;
  devc->mode = 0;
}

#define MIDI_SYNTH_NAME	"MPU-401 UART Midi"
#define MIDI_SYNTH_CAPS	SYNTH_CAP_INPUT
#include "midi_synth.h"

static struct synth_operations mpu401_synth_proto =
{
  NULL,
  0,
  SYNTH_TYPE_MIDI,
  0,
  mpu_synth_open,
  mpu_synth_close,
  mpu_synth_ioctl,
  midi_synth_kill_note,
  midi_synth_start_note,
  midi_synth_set_instr,
  midi_synth_reset,
  midi_synth_hw_control,
  midi_synth_load_patch,
  midi_synth_aftertouch,
  midi_synth_controller,
  midi_synth_panning,
  NULL,
  midi_synth_patchmgr,
  midi_synth_bender,
  NULL,				/* alloc */
  midi_synth_setup_voice,
  midi_synth_send_sysex
};

static struct synth_operations *mpu401_synth_operations[MAX_MIDI_DEV];

static struct midi_operations mpu401_midi_proto =
{
  {"MPU-401 Midi", 0, MIDI_CAP_MPU401, SNDCARD_MPU401},
  NULL,
  {0},
  mpu401_open,
  mpu401_close,
  mpu401_ioctl,
  mpu401_out,
  mpu401_start_read,
  mpu401_end_read,
  mpu401_kick,
  NULL,
  mpu401_buffer_status,
  mpu401_prefix_cmd
};

static struct midi_operations mpu401_midi_operations[MAX_MIDI_DEV];

static void
mpu401_chk_version (struct mpu_config *devc)
{
  int             tmp;
  unsigned long   flags;

  devc->version = devc->revision = 0;

  save_flags (flags);
  cli ();
  if ((tmp = mpu_cmd (num_midis, 0xAC, 0)) < 0)
    {
      restore_flags (flags);
      return;
    }

  if ((tmp & 0xf0) > 0x20)	/* Why it's larger than 2.x ??? */
    {
      restore_flags (flags);
      return;
    }

  devc->version = tmp;

  if ((tmp = mpu_cmd (num_midis, 0xAD, 0)) < 0)
    {
      devc->version = 0;
      restore_flags (flags);
      return;
    }
  devc->revision = tmp;

  restore_flags (flags);
}

void
attach_mpu401 (struct address_info *hw_config)
{
  unsigned long   flags;
  char            revision_char;

  struct mpu_config *devc;

  if (num_midis >= MAX_MIDI_DEV)
    {
      printk ("MPU-401: Too many midi devices detected\n");
      return;
    }

  devc = &dev_conf[num_midis];

  devc->base = hw_config->io_base;
  devc->osp = hw_config->osp;
  devc->irq = hw_config->irq;
  devc->opened = 0;
  devc->uart_mode = 0;
  devc->initialized = 0;
  devc->version = 0;
  devc->revision = 0;
  devc->capabilities = 0;
  devc->timer_flag = 0;
  devc->m_busy = 0;
  devc->m_state = ST_INIT;
  devc->shared_irq = hw_config->always_detect;
  devc->irq = hw_config->irq;

  if (devc->irq < 0)
    {
      devc->irq *= -1;
      devc->shared_irq = 1;
    }
  irq2dev[devc->irq] = num_midis;

  if (!hw_config->always_detect)
    {
      /* Verify the hardware again */
      if (!reset_mpu401 (devc))
	{
	  printk ("MPU401: Device didn't respond\n");
	  return;
	}

      if (!devc->shared_irq)
	if (snd_set_irq_handler (devc->irq, mpuintr, "mpu401", devc->osp) < 0)
	  {
	    printk ("MPU401: Failed to allocate IRQ%d\n", devc->irq);
	    return;
	  }

      save_flags (flags);
      cli ();
      mpu401_chk_version (devc);
      if (devc->version == 0)
	mpu401_chk_version (devc);
      restore_flags (flags);
    }

  request_region (hw_config->io_base, 2, "mpu401");

  if (devc->version != 0)
    if (mpu_cmd (num_midis, 0xC5, 0) >= 0)	/* Set timebase OK */
      if (mpu_cmd (num_midis, 0xE0, 120) >= 0)	/* Set tempo OK */
	devc->capabilities |= MPU_CAP_INTLG;	/* Supports intelligent mode */


  mpu401_synth_operations[num_midis] = (struct synth_operations *) (sound_mem_blocks[sound_nblocks] = vmalloc (sizeof (struct synth_operations)));

  if (sound_nblocks < 1024)
    sound_nblocks++;;

  if (mpu401_synth_operations[num_midis] == NULL)
    {
      printk ("mpu401: Can't allocate memory\n");
      return;
    }

  if (!(devc->capabilities & MPU_CAP_INTLG))	/* No intelligent mode */
    {
      memcpy ((char *) mpu401_synth_operations[num_midis],
	      (char *) &std_midi_synth,
	      sizeof (struct synth_operations));
    }
  else
    {
      memcpy ((char *) mpu401_synth_operations[num_midis],
	      (char *) &mpu401_synth_proto,
	      sizeof (struct synth_operations));
    }

  memcpy ((char *) &mpu401_midi_operations[num_midis],
	  (char *) &mpu401_midi_proto,
	  sizeof (struct midi_operations));

  mpu401_midi_operations[num_midis].converter =
    mpu401_synth_operations[num_midis];

  memcpy ((char *) &mpu_synth_info[num_midis],
	  (char *) &mpu_synth_info_proto,
	  sizeof (struct synth_info));

  n_mpu_devs++;

  if (devc->version == 0x20 && devc->revision >= 0x07)	/* MusicQuest interface */
    {
      int             ports = (devc->revision & 0x08) ? 32 : 16;

      devc->capabilities |= MPU_CAP_SYNC | MPU_CAP_SMPTE |
	MPU_CAP_CLS | MPU_CAP_2PORT;

      revision_char = (devc->revision == 0x7f) ? 'M' : ' ';
      sprintf (mpu_synth_info[num_midis].name,
	       "MQX-%d%c MIDI Interface #%d",
	       ports,
	       revision_char,
	       n_mpu_devs);
    }
  else
    {

      revision_char = devc->revision ? devc->revision + '@' : ' ';
      if ((int) devc->revision > ('Z' - '@'))
	revision_char = '+';

      devc->capabilities |= MPU_CAP_SYNC | MPU_CAP_FSK;

      if (hw_config->name)
	sprintf (mpu_synth_info[num_midis].name, "%s (MPU401)", hw_config->name);
      else
	sprintf (mpu_synth_info[num_midis].name,
		 "MPU-401 %d.%d%c Midi interface #%d",
		 (int) (devc->version & 0xf0) >> 4,
		 devc->version & 0x0f,
		 revision_char,
		 n_mpu_devs);
    }

  strcpy (mpu401_midi_operations[num_midis].info.name,
	  mpu_synth_info[num_midis].name);

  conf_printf (mpu_synth_info[num_midis].name, hw_config);

  mpu401_synth_operations[num_midis]->midi_dev = devc->devno = num_midis;
  mpu401_synth_operations[devc->devno]->info =
    &mpu_synth_info[devc->devno];

  if (devc->capabilities & MPU_CAP_INTLG)	/* Intelligent mode */
    mpu_timer_init (num_midis);

  irq2dev[devc->irq] = num_midis;
  midi_devs[num_midis++] = &mpu401_midi_operations[devc->devno];
}

static int
reset_mpu401 (struct mpu_config *devc)
{
  unsigned long   flags;
  int             ok, timeout, n;
  int             timeout_limit;

  /*
   * Send the RESET command. Try again if no success at the first time.
   * (If the device is in the UART mode, it will not ack the reset cmd).
   */

  ok = 0;

  timeout_limit = devc->initialized ? 30000 : 100000;
  devc->initialized = 1;

  for (n = 0; n < 2 && !ok; n++)
    {
      for (timeout = timeout_limit; timeout > 0 && !ok; timeout--)
	ok = output_ready (devc);

      write_command (devc, MPU_RESET);	/*
					   * Send MPU-401 RESET Command
					 */

      /*
       * Wait at least 25 msec. This method is not accurate so let's make the
       * loop bit longer. Cannot sleep since this is called during boot.
       */

      for (timeout = timeout_limit * 2; timeout > 0 && !ok; timeout--)
	{
	  save_flags (flags);
	  cli ();
	  if (input_avail (devc))
	    if (read_data (devc) == MPU_ACK)
	      ok = 1;
	  restore_flags (flags);
	}

    }

  devc->m_state = ST_INIT;
  devc->m_ptr = 0;
  devc->m_left = 0;
  devc->last_status = 0;
  devc->uart_mode = 0;

  return ok;
}

static void
set_uart_mode (int dev, struct mpu_config *devc, int arg)
{
  if (!arg && (devc->capabilities & MPU_CAP_INTLG))
    {
      return;
    }

  if ((devc->uart_mode == 0) == (arg == 0))
    {
      return;			/* Already set */
    }

  reset_mpu401 (devc);		/* This exits the uart mode */

  if (arg)
    {
      if (mpu_cmd (dev, UART_MODE_ON, 0) < 0)
	{
	  printk ("MPU%d: Can't enter UART mode\n", devc->devno);
	  devc->uart_mode = 0;
	  return;
	}
    }
  devc->uart_mode = arg;

}

int
probe_mpu401 (struct address_info *hw_config)
{
  int             ok = 0;
  struct mpu_config tmp_devc;

  if (check_region (hw_config->io_base, 2))
    {
      printk ("\n\nmpu401.c: I/O port %x already in use\n\n",
	      hw_config->io_base);
      return 0;
    }

  tmp_devc.base = hw_config->io_base;
  tmp_devc.irq = hw_config->irq;
  tmp_devc.initialized = 0;
  tmp_devc.opened = 0;
  tmp_devc.osp = hw_config->osp;

  if (hw_config->always_detect)
    return 1;

  if (inb (hw_config->io_base + 1) == 0xff)
    {
      DDB (printk ("MPU401: Port %x looks dead.\n", hw_config->io_base));
      return 0;			/* Just bus float? */
    }

  ok = reset_mpu401 (&tmp_devc);

  if (!ok)
    {
      DDB (printk ("MPU401: Reset failed on port %x\n", hw_config->io_base));
    }

  return ok;
}

void
unload_mpu401 (struct address_info *hw_config)
{
  release_region (hw_config->io_base, 2);
  if (hw_config->always_detect == 0 && hw_config->irq > 0)
    snd_release_irq (hw_config->irq);
}

/*****************************************************
 *      Timer stuff
 ****************************************************/

#if defined(CONFIG_SEQUENCER)

static volatile int timer_initialized = 0, timer_open = 0, tmr_running = 0;
static volatile int curr_tempo, curr_timebase, hw_timebase;
static int      max_timebase = 8;	/* 8*24=192 ppqn */
static volatile unsigned long next_event_time;
static volatile unsigned long curr_ticks, curr_clocks;
static unsigned long prev_event_time;
static int      metronome_mode;

static unsigned long
clocks2ticks (unsigned long clocks)
{
  /*
     * The MPU-401 supports just a limited set of possible timebase values.
     * Since the applications require more choices, the driver has to
     * program the HW to do it's best and to convert between the HW and
     * actual timebases.
   */

  return ((clocks * curr_timebase) + (hw_timebase / 2)) / hw_timebase;
}

static void
set_timebase (int midi_dev, int val)
{
  int             hw_val;

  if (val < 48)
    val = 48;
  if (val > 1000)
    val = 1000;

  hw_val = val;
  hw_val = (hw_val + 12) / 24;
  if (hw_val > max_timebase)
    hw_val = max_timebase;

  if (mpu_cmd (midi_dev, 0xC0 | (hw_val & 0x0f), 0) < 0)
    {
      printk ("MPU: Can't set HW timebase to %d\n", hw_val * 24);
      return;
    }
  hw_timebase = hw_val * 24;
  curr_timebase = val;

}

static void
tmr_reset (void)
{
  unsigned long   flags;

  save_flags (flags);
  cli ();
  next_event_time = (unsigned long) -1;
  prev_event_time = 0;
  curr_ticks = curr_clocks = 0;
  restore_flags (flags);
}

static void
set_timer_mode (int midi_dev)
{
  if (timer_mode & TMR_MODE_CLS)
    mpu_cmd (midi_dev, 0x3c, 0);	/* Use CLS sync */
  else if (timer_mode & TMR_MODE_SMPTE)
    mpu_cmd (midi_dev, 0x3d, 0);	/* Use SMPTE sync */

  if (timer_mode & TMR_INTERNAL)
    {
      mpu_cmd (midi_dev, 0x80, 0);	/* Use MIDI sync */
    }
  else
    {
      if (timer_mode & (TMR_MODE_MIDI | TMR_MODE_CLS))
	{
	  mpu_cmd (midi_dev, 0x82, 0);	/* Use MIDI sync */
	  mpu_cmd (midi_dev, 0x91, 0);	/* Enable ext MIDI ctrl */
	}
      else if (timer_mode & TMR_MODE_FSK)
	mpu_cmd (midi_dev, 0x81, 0);	/* Use FSK sync */
    }
}

static void
stop_metronome (int midi_dev)
{
  mpu_cmd (midi_dev, 0x84, 0);	/* Disable metronome */
}

static void
setup_metronome (int midi_dev)
{
  int             numerator, denominator;
  int             clks_per_click, num_32nds_per_beat;
  int             beats_per_measure;

  numerator = ((unsigned) metronome_mode >> 24) & 0xff;
  denominator = ((unsigned) metronome_mode >> 16) & 0xff;
  clks_per_click = ((unsigned) metronome_mode >> 8) & 0xff;
  num_32nds_per_beat = (unsigned) metronome_mode & 0xff;
  beats_per_measure = (numerator * 4) >> denominator;

  if (!metronome_mode)
    mpu_cmd (midi_dev, 0x84, 0);	/* Disable metronome */
  else
    {
      mpu_cmd (midi_dev, 0xE4, clks_per_click);
      mpu_cmd (midi_dev, 0xE6, beats_per_measure);
      mpu_cmd (midi_dev, 0x83, 0);	/* Enable metronome without accents */
    }
}

static int
mpu_start_timer (int midi_dev)
{
  tmr_reset ();
  set_timer_mode (midi_dev);

  if (tmr_running)
    return TIMER_NOT_ARMED;	/* Already running */

  if (timer_mode & TMR_INTERNAL)
    {
      mpu_cmd (midi_dev, 0x02, 0);	/* Send MIDI start */
      tmr_running = 1;
      return TIMER_NOT_ARMED;
    }
  else
    {
      mpu_cmd (midi_dev, 0x35, 0);	/* Enable mode messages to PC */
      mpu_cmd (midi_dev, 0x38, 0);	/* Enable sys common messages to PC */
      mpu_cmd (midi_dev, 0x39, 0);	/* Enable real time messages to PC */
      mpu_cmd (midi_dev, 0x97, 0);	/* Enable system exclusive messages to PC */
    }

  return TIMER_ARMED;
}

static int
mpu_timer_open (int dev, int mode)
{
  int             midi_dev = sound_timer_devs[dev]->devlink;

  if (timer_open)
    return -(EBUSY);

  tmr_reset ();
  curr_tempo = 50;
  mpu_cmd (midi_dev, 0xE0, 50);
  curr_timebase = hw_timebase = 120;
  set_timebase (midi_dev, 120);
  timer_open = 1;
  metronome_mode = 0;
  set_timer_mode (midi_dev);

  mpu_cmd (midi_dev, 0xe7, 0x04);	/* Send all clocks to host */
  mpu_cmd (midi_dev, 0x95, 0);	/* Enable clock to host */

  return 0;
}

static void
mpu_timer_close (int dev)
{
  int             midi_dev = sound_timer_devs[dev]->devlink;

  timer_open = tmr_running = 0;
  mpu_cmd (midi_dev, 0x15, 0);	/* Stop all */
  mpu_cmd (midi_dev, 0x94, 0);	/* Disable clock to host */
  mpu_cmd (midi_dev, 0x8c, 0);	/* Disable measure end messages to host */
  stop_metronome (midi_dev);
}

static int
mpu_timer_event (int dev, unsigned char *event)
{
  unsigned char   command = event[1];
  unsigned long   parm = *(unsigned int *) &event[4];
  int             midi_dev = sound_timer_devs[dev]->devlink;

  switch (command)
    {
    case TMR_WAIT_REL:
      parm += prev_event_time;
    case TMR_WAIT_ABS:
      if (parm > 0)
	{
	  long            time;

	  if (parm <= curr_ticks)	/* It's the time */
	    return TIMER_NOT_ARMED;

	  time = parm;
	  next_event_time = prev_event_time = time;

	  return TIMER_ARMED;
	}
      break;

    case TMR_START:
      if (tmr_running)
	break;
      return mpu_start_timer (midi_dev);
      break;

    case TMR_STOP:
      mpu_cmd (midi_dev, 0x01, 0);	/* Send MIDI stop */
      stop_metronome (midi_dev);
      tmr_running = 0;
      break;

    case TMR_CONTINUE:
      if (tmr_running)
	break;
      mpu_cmd (midi_dev, 0x03, 0);	/* Send MIDI continue */
      setup_metronome (midi_dev);
      tmr_running = 1;
      break;

    case TMR_TEMPO:
      if (parm)
	{
	  if (parm < 8)
	    parm = 8;
	  if (parm > 250)
	    parm = 250;

	  if (mpu_cmd (midi_dev, 0xE0, parm) < 0)
	    printk ("MPU: Can't set tempo to %d\n", (int) parm);
	  curr_tempo = parm;
	}
      break;

    case TMR_ECHO:
      seq_copy_to_input (event, 8);
      break;

    case TMR_TIMESIG:
      if (metronome_mode)	/* Metronome enabled */
	{
	  metronome_mode = parm;
	  setup_metronome (midi_dev);
	}
      break;

    default:;
    }

  return TIMER_NOT_ARMED;
}

static unsigned long
mpu_timer_get_time (int dev)
{
  if (!timer_open)
    return 0;

  return curr_ticks;
}

static int
mpu_timer_ioctl (int dev,
		 unsigned int command, caddr_t arg)
{
  int             midi_dev = sound_timer_devs[dev]->devlink;

  switch (command)
    {
    case SNDCTL_TMR_SOURCE:
      {
	int             parm = (int) get_fs_long ((long *) arg) & timer_caps;

	if (parm != 0)
	  {
	    timer_mode = parm;

	    if (timer_mode & TMR_MODE_CLS)
	      mpu_cmd (midi_dev, 0x3c, 0);	/* Use CLS sync */
	    else if (timer_mode & TMR_MODE_SMPTE)
	      mpu_cmd (midi_dev, 0x3d, 0);	/* Use SMPTE sync */
	  }

	return snd_ioctl_return ((int *) arg, timer_mode);
      }
      break;

    case SNDCTL_TMR_START:
      mpu_start_timer (midi_dev);
      return 0;
      break;

    case SNDCTL_TMR_STOP:
      tmr_running = 0;
      mpu_cmd (midi_dev, 0x01, 0);	/* Send MIDI stop */
      stop_metronome (midi_dev);
      return 0;
      break;

    case SNDCTL_TMR_CONTINUE:
      if (tmr_running)
	return 0;
      tmr_running = 1;
      mpu_cmd (midi_dev, 0x03, 0);	/* Send MIDI continue */
      return 0;
      break;

    case SNDCTL_TMR_TIMEBASE:
      {
	int             val = (int) get_fs_long ((long *) arg);

	if (val)
	  set_timebase (midi_dev, val);

	return snd_ioctl_return ((int *) arg, curr_timebase);
      }
      break;

    case SNDCTL_TMR_TEMPO:
      {
	int             val = (int) get_fs_long ((long *) arg);
	int             ret;

	if (val)
	  {
	    if (val < 8)
	      val = 8;
	    if (val > 250)
	      val = 250;
	    if ((ret = mpu_cmd (midi_dev, 0xE0, val)) < 0)
	      {
		printk ("MPU: Can't set tempo to %d\n", (int) val);
		return ret;
	      }

	    curr_tempo = val;
	  }

	return snd_ioctl_return ((int *) arg, curr_tempo);
      }
      break;

    case SNDCTL_SEQ_CTRLRATE:
      if (get_fs_long ((long *) arg) != 0)	/* Can't change */
	return -(EINVAL);

      return snd_ioctl_return ((int *) arg, ((curr_tempo * curr_timebase) + 30) / 60);
      break;

    case SNDCTL_TMR_METRONOME:
      metronome_mode = (int) get_fs_long ((long *) arg);
      setup_metronome (midi_dev);
      return 0;
      break;

    default:;
    }

  return -(EINVAL);
}

static void
mpu_timer_arm (int dev, long time)
{
  if (time < 0)
    time = curr_ticks + 1;
  else if (time <= curr_ticks)	/* It's the time */
    return;

  next_event_time = prev_event_time = time;

  return;
}

static struct sound_timer_operations mpu_timer =
{
  {"MPU-401 Timer", 0},
  10,				/* Priority */
  0,				/* Local device link */
  mpu_timer_open,
  mpu_timer_close,
  mpu_timer_event,
  mpu_timer_get_time,
  mpu_timer_ioctl,
  mpu_timer_arm
};

static void
mpu_timer_interrupt (void)
{

  if (!timer_open)
    return;

  if (!tmr_running)
    return;

  curr_clocks++;
  curr_ticks = clocks2ticks (curr_clocks);

  if (curr_ticks >= next_event_time)
    {
      next_event_time = (unsigned long) -1;
      sequencer_timer (0);
    }
}

static void
timer_ext_event (struct mpu_config *devc, int event, int parm)
{
  int             midi_dev = devc->devno;

  if (!devc->timer_flag)
    return;

  switch (event)
    {
    case TMR_CLOCK:
      printk ("<MIDI clk>");
      break;

    case TMR_START:
      printk ("Ext MIDI start\n");
      if (!tmr_running)
	if (timer_mode & TMR_EXTERNAL)
	  {
	    tmr_running = 1;
	    setup_metronome (midi_dev);
	    next_event_time = 0;
	    STORE (SEQ_START_TIMER ());
	  }
      break;

    case TMR_STOP:
      printk ("Ext MIDI stop\n");
      if (timer_mode & TMR_EXTERNAL)
	{
	  tmr_running = 0;
	  stop_metronome (midi_dev);
	  STORE (SEQ_STOP_TIMER ());
	}
      break;

    case TMR_CONTINUE:
      printk ("Ext MIDI continue\n");
      if (timer_mode & TMR_EXTERNAL)
	{
	  tmr_running = 1;
	  setup_metronome (midi_dev);
	  STORE (SEQ_CONTINUE_TIMER ());
	}
      break;

    case TMR_SPP:
      printk ("Songpos: %d\n", parm);
      if (timer_mode & TMR_EXTERNAL)
	{
	  STORE (SEQ_SONGPOS (parm));
	}
      break;
    }
}

static void
mpu_timer_init (int midi_dev)
{
  struct mpu_config *devc;
  int             n;

  devc = &dev_conf[midi_dev];

  if (timer_initialized)
    return;			/* There is already a similar timer */

  timer_initialized = 1;

  mpu_timer.devlink = midi_dev;
  dev_conf[midi_dev].timer_flag = 1;

  if (num_sound_timers >= MAX_TIMER_DEV)
    n = 0;			/* Overwrite the system timer */
  else
    n = num_sound_timers++;
  sound_timer_devs[n] = &mpu_timer;

  if (devc->version < 0x20)	/* Original MPU-401 */
    timer_caps = TMR_INTERNAL | TMR_EXTERNAL | TMR_MODE_FSK | TMR_MODE_MIDI;
  else
    {
      /*
         * The version number 2.0 is used (at least) by the
         * MusicQuest cards and the Roland Super-MPU.
         *
         * MusicQuest has given a special meaning to the bits of the
         * revision number. The Super-MPU returns 0.
       */

      if (devc->revision)
	timer_caps |= TMR_EXTERNAL | TMR_MODE_MIDI;

      if (devc->revision & 0x02)
	timer_caps |= TMR_MODE_CLS;


      if (devc->revision & 0x40)
	max_timebase = 10;	/* Has the 216 and 240 ppqn modes */
    }

  timer_mode = (TMR_INTERNAL | TMR_MODE_MIDI) & timer_caps;

}

#endif



#endif
