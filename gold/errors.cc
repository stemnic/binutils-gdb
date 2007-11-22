// errors.cc -- handle errors for gold

// Copyright 2006, 2007 Free Software Foundation, Inc.
// Written by Ian Lance Taylor <iant@google.com>.

// This file is part of gold.

// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street - Fifth Floor, Boston,
// MA 02110-1301, USA.

#include "gold.h"

#include <cstdarg>
#include <cstdio>

#include "gold-threads.h"
#include "parameters.h"
#include "object.h"
#include "symtab.h"
#include "errors.h"

namespace gold
{

// Class Errors.

const int Errors::max_undefined_error_report;

Errors::Errors(const char* program_name)
  : program_name_(program_name), lock_(NULL), error_count_(0),
    warning_count_(0), undefined_symbols_()
{
}

// Initialize the lock_ field.

void
Errors::initialize_lock()
{
  if (this->lock_ == NULL)
    this->lock_ = new Lock;
}

// Report a fatal error.

void
Errors::fatal(const char* format, va_list args)
{
  fprintf(stderr, "%s: ", this->program_name_);
  vfprintf(stderr, format, args);
  fputc('\n', stderr);
  gold_exit(false);
}

// Report an error.

void
Errors::error(const char* format, va_list args)
{
  fprintf(stderr, "%s: ", this->program_name_);
  vfprintf(stderr, format, args);
  fputc('\n', stderr);

  this->initialize_lock();
  {
    Hold_lock h(*this->lock_);
    ++this->error_count_;
  }
}

// Report a warning.

void
Errors::warning(const char* format, va_list args)
{
  fprintf(stderr, _("%s: warning: "), this->program_name_);
  vfprintf(stderr, format, args);
  fputc('\n', stderr);

  this->initialize_lock();
  {
    Hold_lock h(*this->lock_);
    ++this->warning_count_;
  }
}

// Report an error at a reloc location.

template<int size, bool big_endian>
void
Errors::error_at_location(const Relocate_info<size, big_endian>* relinfo,
			  size_t relnum, off_t reloffset,
			  const char* format, va_list args)
{
  fprintf(stderr, "%s: %s: ", this->program_name_,
	  relinfo->location(relnum, reloffset).c_str());
  vfprintf(stderr, format, args);
  fputc('\n', stderr);

  this->initialize_lock();
  {
    Hold_lock h(*this->lock_);
    ++this->error_count_;
  }
}

// Report a warning at a reloc location.

template<int size, bool big_endian>
void
Errors::warning_at_location(const Relocate_info<size, big_endian>* relinfo,
			    size_t relnum, off_t reloffset,
			    const char* format, va_list args)
{
  fprintf(stderr, _("%s: %s: warning: "), this->program_name_,
	  relinfo->location(relnum, reloffset).c_str());
  vfprintf(stderr, format, args);
  fputc('\n', stderr);

  this->initialize_lock();
  {
    Hold_lock h(*this->lock_);
    ++this->warning_count_;
  }
}

// Issue an undefined symbol error.

template<int size, bool big_endian>
void
Errors::undefined_symbol(const Symbol* sym,
			 const Relocate_info<size, big_endian>* relinfo,
			 size_t relnum, off_t reloffset)
{
  this->initialize_lock();
  {
    Hold_lock h(*this->lock_);
    if (++this->undefined_symbols_[sym] >= max_undefined_error_report)
      return;
    ++this->error_count_;
  }
  fprintf(stderr, _("%s: %s: undefined reference to '%s'\n"),
	  this->program_name_, relinfo->location(relnum, reloffset).c_str(),
	  sym->demangled_name().c_str());
}

// Issue a debugging message.

void
Errors::debug(const char* format, ...)
{
  fprintf(stderr, _("%s: "), this->program_name_);

  va_list args;
  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);

  fputc('\n', stderr);
}

// The functions which the rest of the code actually calls.

// Report a fatal error.

void
gold_fatal(const char* format, ...)
{
  va_list args;
  va_start(args, format);
  parameters->errors()->fatal(format, args);
  va_end(args);
}

// Report an error.

void
gold_error(const char* format, ...)
{
  va_list args;
  va_start(args, format);
  parameters->errors()->error(format, args);
  va_end(args);
}

// Report a warning.

void
gold_warning(const char* format, ...)
{
  va_list args;
  va_start(args, format);
  parameters->errors()->warning(format, args);
  va_end(args);
}

// Report an error at a location.

template<int size, bool big_endian>
void
gold_error_at_location(const Relocate_info<size, big_endian>* relinfo,
		       size_t relnum, off_t reloffset,
		       const char* format, ...)
{
  va_list args;
  va_start(args, format);
  parameters->errors()->error_at_location(relinfo, relnum, reloffset,
					  format, args);
  va_end(args);
}

// Report a warning at a location.

template<int size, bool big_endian>
void
gold_warning_at_location(const Relocate_info<size, big_endian>* relinfo,
			 size_t relnum, off_t reloffset,
			 const char* format, ...)
{
  va_list args;
  va_start(args, format);
  parameters->errors()->warning_at_location(relinfo, relnum, reloffset,
					    format, args);
  va_end(args);
}

// Report an undefined symbol.

template<int size, bool big_endian>
void
gold_undefined_symbol(const Symbol* sym,
		      const Relocate_info<size, big_endian>* relinfo,
		      size_t relnum, off_t reloffset)
{
  parameters->errors()->undefined_symbol(sym, relinfo, relnum, reloffset);
}

#ifdef HAVE_TARGET_32_LITTLE
template
void
gold_error_at_location<32, false>(const Relocate_info<32, false>* relinfo,
				  size_t relnum, off_t reloffset,
				  const char* format, ...);
#endif

#ifdef HAVE_TARGET_32_BIG
template
void
gold_error_at_location<32, true>(const Relocate_info<32, true>* relinfo,
				 size_t relnum, off_t reloffset,
				 const char* format, ...);
#endif

#ifdef HAVE_TARGET_64_LITTLE
template
void
gold_error_at_location<64, false>(const Relocate_info<64, false>* relinfo,
				  size_t relnum, off_t reloffset,
				  const char* format, ...);
#endif

#ifdef HAVE_TARGET_64_BIG
template
void
gold_error_at_location<64, true>(const Relocate_info<64, true>* relinfo,
				 size_t relnum, off_t reloffset,
				 const char* format, ...);
#endif

#ifdef HAVE_TARGET_32_LITTLE
template
void
gold_warning_at_location<32, false>(const Relocate_info<32, false>* relinfo,
				    size_t relnum, off_t reloffset,
				    const char* format, ...);
#endif

#ifdef HAVE_TARGET_32_BIG
template
void
gold_warning_at_location<32, true>(const Relocate_info<32, true>* relinfo,
				   size_t relnum, off_t reloffset,
				   const char* format, ...);
#endif

#ifdef HAVE_TARGET_64_LITTLE
template
void
gold_warning_at_location<64, false>(const Relocate_info<64, false>* relinfo,
				    size_t relnum, off_t reloffset,
				    const char* format, ...);
#endif

#ifdef HAVE_TARGET_64_BIG
template
void
gold_warning_at_location<64, true>(const Relocate_info<64, true>* relinfo,
				   size_t relnum, off_t reloffset,
				   const char* format, ...);
#endif

#ifdef HAVE_TARGET_32_LITTLE
template
void
gold_undefined_symbol<32, false>(const Symbol* sym,
				 const Relocate_info<32, false>* relinfo,
				 size_t relnum, off_t reloffset);
#endif

#ifdef HAVE_TARGET_32_BIG
template
void
gold_undefined_symbol<32, true>(const Symbol* sym,
				const Relocate_info<32, true>* relinfo,
				size_t relnum, off_t reloffset);
#endif

#ifdef HAVE_TARGET_64_LITTLE
template
void
gold_undefined_symbol<64, false>(const Symbol* sym,
				 const Relocate_info<64, false>* relinfo,
				 size_t relnum, off_t reloffset);
#endif

#ifdef HAVE_TARGET_64_BIG
template
void
gold_undefined_symbol<64, true>(const Symbol* sym,
				const Relocate_info<64, true>* relinfo,
				size_t relnum, off_t reloffset);
#endif

} // End namespace gold.
