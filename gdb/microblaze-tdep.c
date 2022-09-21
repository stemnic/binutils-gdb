/* Target-dependent code for Xilinx MicroBlaze.

   Copyright (C) 2009-2022 Free Software Foundation, Inc.

   This file is part of GDB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#include "defs.h"
#include "arch-utils.h"
#include "dis-asm.h"
#include "frame.h"
#include "trad-frame.h"
#include "symtab.h"
#include "value.h"
#include "gdbcmd.h"
#include "breakpoint.h"
#include "inferior.h"
#include "regcache.h"
#include "target.h"
#include "frame-base.h"
#include "frame-unwind.h"
#include "dwarf2/frame.h"
#include "osabi.h"
#include "target-descriptions.h"
#include "opcodes/microblaze-opcm.h"
#include "opcodes/microblaze-dis.h"
#include "microblaze-tdep.h"
#include "remote.h"

#include "features/microblaze-with-stack-protect.c"
#include "features/microblaze.c"

/* Instruction macros used for analyzing the prologue.  */
/* This set of instruction macros need to be changed whenever the
   prologue generated by the compiler could have more instructions or
   different type of instructions.
   This set also needs to be verified if it is complete.  */
#define IS_RETURN(op) (op == rtsd || op == rtid)
#define IS_UPDATE_SP(op, rd, ra) \
  ((op == addik || op == addi) && rd == REG_SP && ra == REG_SP)
#define IS_SPILL_SP(op, rd, ra) \
  ((op == swi || op == sw) && rd == REG_SP && ra == REG_SP)
#define IS_SPILL_REG(op, rd, ra) \
  ((op == swi || op == sw) && rd != REG_SP && ra == REG_SP)
#define IS_ALSO_SPILL_REG(op, rd, ra, rb) \
  ((op == swi || op == sw) && rd != REG_SP && ra == 0 && rb == REG_SP)
#define IS_SETUP_FP(op, ra, rb) \
  ((op == add || op == addik || op == addk) && ra == REG_SP && rb == 0)
#define IS_SPILL_REG_FP(op, rd, ra, fpregnum) \
  ((op == swi || op == sw) && rd != REG_SP && ra == fpregnum && ra != 0)
#define IS_SAVE_HIDDEN_PTR(op, rd, ra, rb) \
  ((op == add || op == addik) && ra == MICROBLAZE_FIRST_ARGREG && rb == 0)

/* The registers of the Xilinx microblaze processor.  */

static const char * const microblaze_register_names[] =
{
  "r0",   "r1",  "r2",    "r3",   "r4",   "r5",   "r6",   "r7",
  "r8",   "r9",  "r10",   "r11",  "r12",  "r13",  "r14",  "r15",
  "r16",  "r17", "r18",   "r19",  "r20",  "r21",  "r22",  "r23",
  "r24",  "r25", "r26",   "r27",  "r28",  "r29",  "r30",  "r31",
  "rpc",  "rmsr", "rear", "resr", "rfsr", "rbtr",
  "rpvr0", "rpvr1", "rpvr2", "rpvr3", "rpvr4", "rpvr5", "rpvr6",
  "rpvr7", "rpvr8", "rpvr9", "rpvr10", "rpvr11",
  "redr", "rpid", "rzpr", "rtlbx", "rtlbsx", "rtlblo", "rtlbhi",
  "rslr", "rshr"
};

#define MICROBLAZE_NUM_REGS ARRAY_SIZE (microblaze_register_names)

static unsigned int microblaze_debug_flag = 0;

#define microblaze_debug(fmt, ...) \
  debug_prefixed_printf_cond_nofunc (microblaze_debug_flag, "MICROBLAZE", \
				     fmt, ## __VA_ARGS__)


/* Return the name of register REGNUM.  */

static const char *
microblaze_register_name (struct gdbarch *gdbarch, int regnum)
{
  if (regnum >= 0 && regnum < MICROBLAZE_NUM_REGS)
    return microblaze_register_names[regnum];
  return NULL;
}

static struct type *
microblaze_register_type (struct gdbarch *gdbarch, int regnum)
{
  if (regnum == MICROBLAZE_SP_REGNUM)
    return builtin_type (gdbarch)->builtin_data_ptr;

  if (regnum == MICROBLAZE_PC_REGNUM)
    return builtin_type (gdbarch)->builtin_func_ptr;

  return builtin_type (gdbarch)->builtin_int;
}


/* Fetch the instruction at PC.  */

static unsigned long
microblaze_fetch_instruction (CORE_ADDR pc)
{
  enum bfd_endian byte_order = gdbarch_byte_order (target_gdbarch ());
  gdb_byte buf[4];

  /* If we can't read the instruction at PC, return zero.  */
  if (target_read_code (pc, buf, sizeof (buf)))
    return 0;

  return extract_unsigned_integer (buf, 4, byte_order);
}

constexpr gdb_byte microblaze_break_insn[] = MICROBLAZE_BREAKPOINT;

typedef BP_MANIPULATION (microblaze_break_insn) microblaze_breakpoint;


/* Allocate and initialize a frame cache.  */

static struct microblaze_frame_cache *
microblaze_alloc_frame_cache (void)
{
  struct microblaze_frame_cache *cache;

  cache = FRAME_OBSTACK_ZALLOC (struct microblaze_frame_cache);

  /* Base address.  */
  cache->base = 0;
  cache->pc = 0;

  /* Frameless until proven otherwise.  */
  cache->frameless_p = 1;

  return cache;
}

/* The base of the current frame is actually in the stack pointer.
   This happens when there is no frame pointer (microblaze ABI does not
   require a frame pointer) or when we're stopped in the prologue or
   epilogue itself.  In these cases, microblaze_analyze_prologue will need
   to update fi->frame before returning or analyzing the register
   save instructions.  */
#define MICROBLAZE_MY_FRAME_IN_SP 0x1

/* The base of the current frame is in a frame pointer register.
   This register is noted in frame_extra_info->fp_regnum.

   Note that the existance of an FP might also indicate that the
   function has called alloca.  */
#define MICROBLAZE_MY_FRAME_IN_FP 0x2

/* Function prologues on the Xilinx microblaze processors consist of:

   - adjustments to the stack pointer (r1) (addi r1, r1, imm)
   - making a copy of r1 into another register (a "frame" pointer)
     (add r?, r1, r0)
   - store word/multiples that use r1 or the frame pointer as the
     base address (swi r?, r1, imm OR swi r?, fp, imm)

   Note that microblaze really doesn't have a real frame pointer.
   Instead, the compiler may copy the SP into a register (usually
   r19) to act as an arg pointer.  For our target-dependent purposes,
   the frame info's "frame" member will be the beginning of the
   frame.  The SP could, in fact, point below this.

   The prologue ends when an instruction fails to meet either of
   these criteria.  */

/* Analyze the prologue to determine where registers are saved,
   the end of the prologue, etc.  Return the address of the first line
   of "real" code (i.e., the end of the prologue).  */

static CORE_ADDR
microblaze_analyze_prologue (struct gdbarch *gdbarch, CORE_ADDR pc, 
			     CORE_ADDR current_pc,
			     struct microblaze_frame_cache *cache)
{
  const char *name;
  CORE_ADDR func_addr, func_end, addr, stop, prologue_end_addr = 0;
  unsigned long insn;
  int rd, ra, rb, imm;
  enum microblaze_instr op;
  int save_hidden_pointer_found = 0;
  int non_stack_instruction_found = 0;

  /* Find the start of this function.  */
  find_pc_partial_function (pc, &name, &func_addr, &func_end);
  if (func_addr < pc)
    pc = func_addr;

  if (current_pc < pc)
    return current_pc;

   /* Initialize info about frame.  */
   cache->framesize = 0;
   cache->fp_regnum = MICROBLAZE_SP_REGNUM;
   cache->frameless_p = 1;

  /* Start decoding the prologue.  We start by checking two special cases:

     1. We're about to return
     2. We're at the first insn of the prologue.

     If we're about to return, our frame has already been deallocated.
     If we are stopped at the first instruction of a prologue,
     then our frame has not yet been set up.  */

  /* Get the first insn from memory.  */

  insn = microblaze_fetch_instruction (pc);
  op = microblaze_decode_insn (insn, &rd, &ra, &rb, &imm);

  if (IS_RETURN(op))
    return pc;

  /* Start at beginning of function and analyze until we get to the
     current pc, or the end of the function, whichever is first.  */
  stop = (current_pc < func_end ? current_pc : func_end);

  microblaze_debug ("Scanning prologue: name=%s, func_addr=%s, stop=%s\n", 
		    name, paddress (gdbarch, func_addr), 
		    paddress (gdbarch, stop));

  for (addr = func_addr; addr < stop; addr += INST_WORD_SIZE)
    {
      insn = microblaze_fetch_instruction (addr);
      op = microblaze_decode_insn (insn, &rd, &ra, &rb, &imm);
      microblaze_debug ("%s %08lx\n", paddress (gdbarch, pc), insn);

      /* This code is very sensitive to what functions are present in the
	 prologue.  It assumes that the (addi, addik, swi, sw) can be the 
	 only instructions in the prologue.  */
      if (IS_UPDATE_SP(op, rd, ra))
	{
	  microblaze_debug ("got addi r1,r1,%d; contnuing\n", imm);
	  if (cache->framesize)
	    break;	/* break if framesize already computed.  */
	  cache->framesize = -imm; /* stack grows towards low memory.  */
	  cache->frameless_p = 0; /* Frame found.  */
	  save_hidden_pointer_found = 0;
	  non_stack_instruction_found = 0;
	  continue;
	}
      else if (IS_SPILL_SP(op, rd, ra))
	{
	  /* Spill stack pointer.  */
	  cache->register_offsets[rd] = imm; /* SP spilled before updating.  */

	  microblaze_debug ("swi r1 r1 %d, continuing\n", imm);
	  save_hidden_pointer_found = 0;
	  if (!cache->framesize)
	    non_stack_instruction_found = 0;
	  continue;
	}
      else if (IS_SPILL_REG(op, rd, ra))
	{
	  /* Spill register.  */
	  cache->register_offsets[rd] = imm - cache->framesize;

	  microblaze_debug ("swi %d r1 %d, continuing\n", rd, imm);
	  save_hidden_pointer_found = 0;
	  if (!cache->framesize)
	    non_stack_instruction_found = 0;
	  continue;
	}
      else if (IS_ALSO_SPILL_REG(op, rd, ra, rb))
	{
	  /* Spill register.  */
	  cache->register_offsets[rd] = 0 - cache->framesize;

	  microblaze_debug ("sw %d r0 r1, continuing\n", rd);
	  save_hidden_pointer_found = 0;
	  if (!cache->framesize)
	    non_stack_instruction_found = 0;
	  continue;
	}
      else if (IS_SETUP_FP(op, ra, rb))
	{
	  /* We have a frame pointer.  Note the register which is 
	     acting as the frame pointer.  */
	  cache->fp_regnum = rd;
	  microblaze_debug ("Found a frame pointer: r%d\n", cache->fp_regnum);
	  save_hidden_pointer_found = 0;
	  if (!cache->framesize)
	    non_stack_instruction_found = 0;
	  continue;
	}
      else if (IS_SPILL_REG_FP(op, rd, ra, cache->fp_regnum))
	{
	  /* reg spilled after updating.  */
	  cache->register_offsets[rd] = imm - cache->framesize;

	  microblaze_debug ("swi %d %d %d, continuing\n", rd, ra, imm);
	  save_hidden_pointer_found = 0;
	  if (!cache->framesize)
	    non_stack_instruction_found = 0;
	  continue;
	}
      else if (IS_SAVE_HIDDEN_PTR(op, rd, ra, rb))
	{
	  /* If the first argument is a hidden pointer to the area where the
	     return structure is to be saved, then it is saved as part of the
	     prologue.  */

	  microblaze_debug ("add %d %d %d, continuing\n", rd, ra, rb);
	  save_hidden_pointer_found = 1;
	  if (!cache->framesize)
	    non_stack_instruction_found = 0;
	  continue;
	}

      /* As a result of the modification in the next step where we continue
	 to analyze the prologue till we reach a control flow instruction,
	 we need another variable to store when exactly a non-stack
	 instruction was encountered, which is the current definition
	 of a prologue.  */
      if (!non_stack_instruction_found)
	prologue_end_addr = addr;
      non_stack_instruction_found = 1;

      /* When optimizations are enabled, it is not guaranteed that prologue
	 instructions are not mixed in with other instructions from the
	 program.  Some programs show this behavior at -O2.  This can be
	 avoided by adding -fno-schedule-insns2 switch as of now (edk 8.1)
	 In such cases, we scan the function until we see the first control
	 instruction.  */

      {
	unsigned ctrl_op = (unsigned)insn >> 26;

	/* continue if not control flow (branch, return).  */
	if (ctrl_op != 0x26 && ctrl_op != 0x27 && ctrl_op != 0x2d
	    && ctrl_op != 0x2e && ctrl_op != 0x2f)
	  continue;
	else if (ctrl_op == 0x2c)
	  continue;    /* continue if imm.  */
      }

      /* This is not a prologue insn, so stop here.  */
      microblaze_debug ("insn is not a prologue insn -- ending scan\n");
      break;
    }

  microblaze_debug ("done analyzing prologue\n");
  microblaze_debug ("prologue end = 0x%x\n", (int) addr);

  /* If the last instruction was an add rd, r5, r0 then don't count it as
     part of the prologue.  */
  if (save_hidden_pointer_found)
    prologue_end_addr -= INST_WORD_SIZE;

  return prologue_end_addr;
}

static CORE_ADDR
microblaze_unwind_pc (struct gdbarch *gdbarch, struct frame_info *next_frame)
{
  gdb_byte buf[4];
  CORE_ADDR pc;

  frame_unwind_register (next_frame, MICROBLAZE_PC_REGNUM, buf);
  pc = extract_typed_address (buf, builtin_type (gdbarch)->builtin_func_ptr);
  /* For sentinel frame, return address is actual PC.  For other frames,
     return address is pc+8.  This is a workaround because gcc does not
     generate correct return address in CIE.  */
  if (frame_relative_level (next_frame) >= 0)
    pc += 8;
  return pc;
}

/* Return PC of first real instruction of the function starting at
   START_PC.  */

static CORE_ADDR
microblaze_skip_prologue (struct gdbarch *gdbarch, CORE_ADDR start_pc)
{
  struct symtab_and_line sal;
  CORE_ADDR func_start, func_end, ostart_pc;
  struct microblaze_frame_cache cache;

  /* This is the preferred method, find the end of the prologue by
     using the debugging information.  Debugging info does not always
     give the right answer since parameters are stored on stack after this.
     Always analyze the prologue.  */
  if (find_pc_partial_function (start_pc, NULL, &func_start, &func_end))
    {
      sal = find_pc_line (func_start, 0);

      if (sal.end < func_end
	  && start_pc <= sal.end)
	start_pc = sal.end;
    }

  ostart_pc = microblaze_analyze_prologue (gdbarch, func_start, 0xffffffffUL, 
					   &cache);

  if (ostart_pc > start_pc)
    return ostart_pc;
  return start_pc;
}

/* Normal frames.  */

static struct microblaze_frame_cache *
microblaze_frame_cache (struct frame_info *next_frame, void **this_cache)
{
  struct microblaze_frame_cache *cache;
  struct gdbarch *gdbarch = get_frame_arch (next_frame);
  int rn;

  if (*this_cache)
    return (struct microblaze_frame_cache *) *this_cache;

  cache = microblaze_alloc_frame_cache ();
  *this_cache = cache;
  cache->saved_regs = trad_frame_alloc_saved_regs (next_frame);

  /* Clear offsets to saved regs in frame.  */
  for (rn = 0; rn < gdbarch_num_regs (gdbarch); rn++)
    cache->register_offsets[rn] = -1;

  /* Call for side effects.  */
  get_frame_func (next_frame);

  cache->pc = get_frame_address_in_block (next_frame);

  return cache;
}

static void
microblaze_frame_this_id (struct frame_info *next_frame, void **this_cache,
		       struct frame_id *this_id)
{
  struct microblaze_frame_cache *cache =
    microblaze_frame_cache (next_frame, this_cache);

  /* This marks the outermost frame.  */
  if (cache->base == 0)
    return;

  (*this_id) = frame_id_build (cache->base, cache->pc);
}

static struct value *
microblaze_frame_prev_register (struct frame_info *this_frame,
				 void **this_cache, int regnum)
{
  struct microblaze_frame_cache *cache =
    microblaze_frame_cache (this_frame, this_cache);

  if (cache->frameless_p)
    {
      if (regnum == MICROBLAZE_PC_REGNUM)
	regnum = 15;
      if (regnum == MICROBLAZE_SP_REGNUM)
	regnum = 1;
      return trad_frame_get_prev_register (this_frame,
					   cache->saved_regs, regnum);
    }
  else
    return trad_frame_get_prev_register (this_frame, cache->saved_regs,
					 regnum);

}

static const struct frame_unwind microblaze_frame_unwind =
{
  "microblaze prologue",
  NORMAL_FRAME,
  default_frame_unwind_stop_reason,
  microblaze_frame_this_id,
  microblaze_frame_prev_register,
  NULL,
  default_frame_sniffer
};

static CORE_ADDR
microblaze_frame_base_address (struct frame_info *next_frame,
			       void **this_cache)
{
  struct microblaze_frame_cache *cache =
    microblaze_frame_cache (next_frame, this_cache);

  return cache->base;
}

static const struct frame_base microblaze_frame_base =
{
  &microblaze_frame_unwind,
  microblaze_frame_base_address,
  microblaze_frame_base_address,
  microblaze_frame_base_address
};

/* Extract from an array REGBUF containing the (raw) register state, a
   function return value of TYPE, and copy that into VALBUF.  */
static void
microblaze_extract_return_value (struct type *type, struct regcache *regcache,
				 gdb_byte *valbuf)
{
  gdb_byte buf[8];

  /* Copy the return value (starting) in RETVAL_REGNUM to VALBUF.  */
  switch (type->length ())
    {
      case 1:	/* return last byte in the register.  */
	regcache->cooked_read (MICROBLAZE_RETVAL_REGNUM, buf);
	memcpy(valbuf, buf + MICROBLAZE_REGISTER_SIZE - 1, 1);
	return;
      case 2:	/* return last 2 bytes in register.  */
	regcache->cooked_read (MICROBLAZE_RETVAL_REGNUM, buf);
	memcpy(valbuf, buf + MICROBLAZE_REGISTER_SIZE - 2, 2);
	return;
      case 4:	/* for sizes 4 or 8, copy the required length.  */
      case 8:
	regcache->cooked_read (MICROBLAZE_RETVAL_REGNUM, buf);
	regcache->cooked_read (MICROBLAZE_RETVAL_REGNUM + 1, buf+4);
	memcpy (valbuf, buf, type->length ());
	return;
      default:
	internal_error (__FILE__, __LINE__, 
			_("Unsupported return value size requested"));
    }
}

/* Store the return value in VALBUF (of type TYPE) where the caller
   expects to see it.

   Integers up to four bytes are stored in r3.

   Longs are stored in r3 (most significant word) and r4 (least
   significant word).

   Small structures are always returned on stack.  */

static void
microblaze_store_return_value (struct type *type, struct regcache *regcache,
			       const gdb_byte *valbuf)
{
  int len = type->length ();
  gdb_byte buf[8];

  memset (buf, 0, sizeof(buf));

  /* Integral and pointer return values.  */

  if (len > 4)
    {
       gdb_assert (len == 8);
       memcpy (buf, valbuf, 8);
       regcache->cooked_write (MICROBLAZE_RETVAL_REGNUM+1, buf + 4);
    }
  else
    /* ??? Do we need to do any sign-extension here?  */
    memcpy (buf + 4 - len, valbuf, len);

  regcache->cooked_write (MICROBLAZE_RETVAL_REGNUM, buf);
}

static enum return_value_convention
microblaze_return_value (struct gdbarch *gdbarch, struct value *function,
			 struct type *type, struct regcache *regcache,
			 gdb_byte *readbuf, const gdb_byte *writebuf)
{
  if (readbuf)
    microblaze_extract_return_value (type, regcache, readbuf);
  if (writebuf)
    microblaze_store_return_value (type, regcache, writebuf);

  return RETURN_VALUE_REGISTER_CONVENTION;
}

static int
microblaze_stabs_argument_has_addr (struct gdbarch *gdbarch, struct type *type)
{
  return (type->length () == 16);
}


static int dwarf2_to_reg_map[78] =
{ 0  /* r0  */,   1  /* r1  */,   2  /* r2  */,   3  /* r3  */,  /*  0- 3 */
  4  /* r4  */,   5  /* r5  */,   6  /* r6  */,   7  /* r7  */,  /*  4- 7 */
  8  /* r8  */,   9  /* r9  */,  10  /* r10 */,  11  /* r11 */,  /*  8-11 */
  12 /* r12 */,  13  /* r13 */,  14  /* r14 */,  15  /* r15 */,  /* 12-15 */
  16 /* r16 */,  17  /* r17 */,  18  /* r18 */,  19  /* r19 */,  /* 16-19 */
  20 /* r20 */,  21  /* r21 */,  22  /* r22 */,  23  /* r23 */,  /* 20-23 */
  24 /* r24 */,  25  /* r25 */,  26  /* r26 */,  27  /* r27 */,  /* 24-25 */
  28 /* r28 */,  29  /* r29 */,  30  /* r30 */,  31  /* r31 */,  /* 28-31 */
  -1 /* $f0 */,  -1  /* $f1 */,  -1  /* $f2 */,  -1  /* $f3 */,  /* 32-35 */
  -1 /* $f4 */,  -1  /* $f5 */,  -1  /* $f6 */,  -1  /* $f7 */,  /* 36-39 */
  -1 /* $f8 */,  -1  /* $f9 */,  -1  /* $f10 */, -1  /* $f11 */, /* 40-43 */
  -1 /* $f12 */, -1  /* $f13 */, -1  /* $f14 */, -1  /* $f15 */, /* 44-47 */
  -1 /* $f16 */, -1  /* $f17 */, -1  /* $f18 */, -1  /* $f19 */, /* 48-51 */
  -1 /* $f20 */, -1  /* $f21 */, -1  /* $f22 */, -1  /* $f23 */, /* 52-55 */
  -1 /* $f24 */, -1  /* $f25 */, -1  /* $f26 */, -1  /* $f27 */, /* 56-59 */
  -1 /* $f28 */, -1  /* $f29 */, -1  /* $f30 */, -1  /* $f31 */, /* 60-63 */
  -1 /* hi   */, -1  /* lo   */, -1  /* accum*/, 33  /* rmsr */, /* 64-67 */
  -1 /* $fcc1*/, -1  /* $fcc2*/, -1  /* $fcc3*/, -1  /* $fcc4*/, /* 68-71 */
  -1 /* $fcc5*/, -1  /* $fcc6*/, -1  /* $fcc7*/, -1  /* $ap  */, /* 72-75 */
  -1 /* $rap */, -1  /* $frp */					 /* 76-77 */
};

static int
microblaze_dwarf2_reg_to_regnum (struct gdbarch *gdbarch, int reg)
{
  if (reg >= 0 && reg < sizeof (dwarf2_to_reg_map))
    return dwarf2_to_reg_map[reg];
  return -1;
}

static void
microblaze_register_g_packet_guesses (struct gdbarch *gdbarch)
{
  register_remote_g_packet_guess (gdbarch,
				  4 * MICROBLAZE_NUM_CORE_REGS,
				  tdesc_microblaze);

  register_remote_g_packet_guess (gdbarch,
				  4 * MICROBLAZE_NUM_REGS,
				  tdesc_microblaze_with_stack_protect);
}

static struct gdbarch *
microblaze_gdbarch_init (struct gdbarch_info info, struct gdbarch_list *arches)
{
  struct gdbarch *gdbarch;
  tdesc_arch_data_up tdesc_data;
  const struct target_desc *tdesc = info.target_desc;

  /* If there is already a candidate, use it.  */
  arches = gdbarch_list_lookup_by_info (arches, &info);
  if (arches != NULL)
    return arches->gdbarch;
  if (tdesc == NULL)
    tdesc = tdesc_microblaze;

  /* Check any target description for validity.  */
  if (tdesc_has_registers (tdesc))
    {
      const struct tdesc_feature *feature;
      int valid_p;
      int i;

      feature = tdesc_find_feature (tdesc,
				    "org.gnu.gdb.microblaze.core");
      if (feature == NULL)
	return NULL;
      tdesc_data = tdesc_data_alloc ();

      valid_p = 1;
      for (i = 0; i < MICROBLAZE_NUM_CORE_REGS; i++)
	valid_p &= tdesc_numbered_register (feature, tdesc_data.get (), i,
					    microblaze_register_names[i]);
      feature = tdesc_find_feature (tdesc,
				    "org.gnu.gdb.microblaze.stack-protect");
      if (feature != NULL)
	{
	  valid_p = 1;
	  valid_p &= tdesc_numbered_register (feature, tdesc_data.get (),
					      MICROBLAZE_SLR_REGNUM,
					      "rslr");
	  valid_p &= tdesc_numbered_register (feature, tdesc_data.get (),
					      MICROBLAZE_SHR_REGNUM,
					      "rshr");
	}

      if (!valid_p)
	return NULL;
    }

  /* Allocate space for the new architecture.  */
  microblaze_gdbarch_tdep *tdep = new microblaze_gdbarch_tdep;
  gdbarch = gdbarch_alloc (&info, tdep);

  set_gdbarch_long_double_bit (gdbarch, 128);

  set_gdbarch_num_regs (gdbarch, MICROBLAZE_NUM_REGS);
  set_gdbarch_register_name (gdbarch, microblaze_register_name);
  set_gdbarch_register_type (gdbarch, microblaze_register_type);

  /* Register numbers of various important registers.  */
  set_gdbarch_sp_regnum (gdbarch, MICROBLAZE_SP_REGNUM); 
  set_gdbarch_pc_regnum (gdbarch, MICROBLAZE_PC_REGNUM); 

  /* Map Dwarf2 registers to GDB registers.  */
  set_gdbarch_dwarf2_reg_to_regnum (gdbarch, microblaze_dwarf2_reg_to_regnum);

  /* Call dummy code.  */
  set_gdbarch_call_dummy_location (gdbarch, ON_STACK);

  set_gdbarch_return_value (gdbarch, microblaze_return_value);
  set_gdbarch_stabs_argument_has_addr
    (gdbarch, microblaze_stabs_argument_has_addr);

  set_gdbarch_skip_prologue (gdbarch, microblaze_skip_prologue);

  /* Stack grows downward.  */
  set_gdbarch_inner_than (gdbarch, core_addr_lessthan);

  set_gdbarch_breakpoint_kind_from_pc (gdbarch,
				       microblaze_breakpoint::kind_from_pc);
  set_gdbarch_sw_breakpoint_from_kind (gdbarch,
				       microblaze_breakpoint::bp_from_kind);

  set_gdbarch_frame_args_skip (gdbarch, 8);

  set_gdbarch_unwind_pc (gdbarch, microblaze_unwind_pc);

  microblaze_register_g_packet_guesses (gdbarch);

  frame_base_set_default (gdbarch, &microblaze_frame_base);

  /* Hook in ABI-specific overrides, if they have been registered.  */
  gdbarch_init_osabi (info, gdbarch);

  /* Unwind the frame.  */
  dwarf2_append_unwinders (gdbarch);
  frame_unwind_append_unwinder (gdbarch, &microblaze_frame_unwind);
  frame_base_append_sniffer (gdbarch, dwarf2_frame_base_sniffer);
  if (tdesc_data != NULL)
    tdesc_use_registers (gdbarch, tdesc, std::move (tdesc_data));

  return gdbarch;
}

void _initialize_microblaze_tdep ();
void
_initialize_microblaze_tdep ()
{
  gdbarch_register (bfd_arch_microblaze, microblaze_gdbarch_init);

  initialize_tdesc_microblaze_with_stack_protect ();
  initialize_tdesc_microblaze ();
  /* Debug this files internals.  */
  add_setshow_zuinteger_cmd ("microblaze", class_maintenance,
			     &microblaze_debug_flag, _("\
Set microblaze debugging."), _("\
Show microblaze debugging."), _("\
When non-zero, microblaze specific debugging is enabled."),
			     NULL,
			     NULL,
			     &setdebuglist, &showdebuglist);

}
