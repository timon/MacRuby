/* 
 * MacRuby Strings.
 *
 * This file is covered by the Ruby license. See COPYING for more details.
 * 
 * Copyright (C) 2007-2010, Apple Inc. All rights reserved.
 * Copyright (C) 1993-2007 Yukihiro Matsumoto
 * Copyright (C) 2000 Network Applied Communication Laboratory, Inc.
 * Copyright (C) 2000 Information-technology Promotion Agency, Japan
 */

#include <stdio.h>
#include <stdarg.h>
#include <wctype.h>

#include "ruby.h"
#include "ruby/encoding.h"
#include "encoding.h"
#include "re.h"
#include "objc.h"
#include "id.h"
#include "ruby/node.h"
#include "vm.h"

VALUE rb_cSymbol; 	// XXX move me outside
VALUE rb_cByteString; 	// XXX remove all references about me, i'm dead

VALUE rb_cString;
VALUE rb_cNSString;
VALUE rb_cNSMutableString;
VALUE rb_cRubyString;

VALUE rb_fs;

// rb_str_t primitives.

static void
str_update_flags_utf16(rb_str_t *self)
{
    assert(str_is_stored_in_uchars(self)
	    || NON_NATIVE_UTF16_ENC(self->encoding));

    bool ascii_only = true;
    bool has_supplementary = false;
    bool valid_encoding = true;
    // if the length is an odd number, it can't be valid UTF-16
    if (ODD_NUMBER(self->length_in_bytes)) {
	valid_encoding = false;
    }

    UChar *uchars = self->data.uchars;
    long uchars_count = BYTES_TO_UCHARS(self->length_in_bytes);
    bool native_byte_order = str_is_stored_in_uchars(self);
    UChar32 lead = 0;
    for (int i = 0; i < uchars_count; ++i) {
	UChar32 c;
	if (native_byte_order) {
	    c = uchars[i];
	}
	else {
	    uint8_t *bytes = (uint8_t *)&uchars[i];
	    c = (uint16_t)bytes[0] << 8 | (uint16_t)bytes[1];
	}
	if (U16_IS_SURROGATE(c)) { // surrogate
	    if (U16_IS_SURROGATE_LEAD(c)) { // lead surrogate
		// a lead surrogate should not be
		// after an other lead surrogate
		if (lead != 0) {
		    valid_encoding = false;
		}
		lead = c;
	    }
	    else { // trail surrogate
		// a trail surrogate must follow a lead surrogate
		if (lead == 0) {
		    valid_encoding = false;
		}
		else {
		    has_supplementary = true;
		    c = U16_GET_SUPPLEMENTARY(lead, c);
		    if (!U_IS_UNICODE_CHAR(c)) {
			valid_encoding = false;
		    }
		}
		lead = 0;
	    }
	}
	else { // not a surrogate
	    // a non-surrogate character should not be after a lead surrogate
	    // and it should be a valid Unicode character
	    // Warning: Ruby 1.9 does not do the IS_UNICODE_CHAR check
	    // (for 1.9, 0xffff is valid though it's not a Unicode character)
	    if ((lead != 0) || !U_IS_UNICODE_CHAR(c)) {
		valid_encoding = false;
	    }

	    if (c > 127) {
		ascii_only = false;
	    }
	}
    }
    // the last character should not be a lead surrogate
    if (lead != 0) {
	valid_encoding = false;
    }

    str_set_has_supplementary(self, has_supplementary);
    if (valid_encoding) {
	str_set_valid_encoding(self, true);
	str_set_ascii_only(self, ascii_only);
    }
    else {
	str_set_valid_encoding(self, false);
	str_set_ascii_only(self, false);
    }
}

void
str_update_flags(rb_str_t *self)
{
    if (self->length_in_bytes == 0) {
	str_set_valid_encoding(self, true);
	str_set_ascii_only(self, true);
	str_set_has_supplementary(self, false);
    }
    else if (BINARY_ENC(self->encoding)) {
	str_set_valid_encoding(self, true);
	str_set_has_supplementary(self, false);
	bool ascii_only = true;
	for (long i = 0; i < self->length_in_bytes; ++i) {
	    if ((uint8_t)self->data.bytes[i] > 127) {
		ascii_only = false;
		break;
	    }
	}
	str_set_ascii_only(self, ascii_only);
    }
    else if (str_is_stored_in_uchars(self) || UTF16_ENC(self->encoding)) {
	str_update_flags_utf16(self);
    }
    else {
	self->encoding->methods.update_flags(self);
    }
}

static void
str_invert_byte_order(rb_str_t *self)
{
    assert(NON_NATIVE_UTF16_ENC(self->encoding));

    long length_in_bytes = self->length_in_bytes;
    char *bytes = self->data.bytes;

    if (ODD_NUMBER(length_in_bytes)) {
	--length_in_bytes;
    }

    for (long i = 0; i < length_in_bytes; i += 2) {
	char tmp = bytes[i];
	bytes[i] = bytes[i+1];
	bytes[i+1] = tmp;
    }
    str_negate_stored_in_uchars(self);
}

static rb_encoding_t *
str_compatible_encoding(rb_str_t *str1, rb_str_t *str2)
{
    if (str1->encoding == str2->encoding) {
	return str1->encoding;
    }
    if (str2->length_in_bytes == 0) {
	return str1->encoding;
    }
    if (str1->length_in_bytes == 0) {
	return str2->encoding;
    }
    if (!str1->encoding->ascii_compatible
	    || !str2->encoding->ascii_compatible) {
	return NULL;
    }
    if (str_is_ruby_ascii_only(str2)) {
	return str1->encoding;
    }
    return NULL;
}

static rb_encoding_t *
str_must_have_compatible_encoding(rb_str_t *str1, rb_str_t *str2)
{
    rb_encoding_t *new_encoding = str_compatible_encoding(str1, str2);
    if (new_encoding == NULL) {
	rb_raise(rb_eEncCompatError,
		"incompatible character encodings: %s and %s",
		str1->encoding->public_name, str2->encoding->public_name);
    }
    return new_encoding;
}

static rb_str_t *
str_alloc(VALUE klass)
{
    assert(rb_klass_is_rstr(klass));
    assert(klass != 0);

    NEWOBJ(str, rb_str_t);
    str->basic.flags = 0;
    str->basic.klass = klass;
    str->encoding = rb_encodings[ENCODING_BINARY];
    str->capacity_in_bytes = 0;
    str->length_in_bytes = 0;
    str->data.bytes = NULL;
    str->flags = 0;
    return str;
}

static VALUE
str_new(void)
{
    return (VALUE)str_alloc(rb_cRubyString);
}

static void
str_replace_with_bytes(rb_str_t *self, const char *bytes, long len,
	rb_encoding_t *enc)
{
    assert(len >= 0);
    assert(enc != NULL);

    self->flags = 0;
    self->encoding = enc;
    self->capacity_in_bytes = len;
    if (len > 0) {
	GC_WB(&self->data.bytes, xmalloc(len));
	if (bytes != NULL) {
	    memcpy(self->data.bytes, bytes, len);
	    self->length_in_bytes = len;
	}
	else {
	    self->length_in_bytes = 0;
	}
    }
    else {
	self->data.bytes = NULL;
	self->length_in_bytes = 0;
    }
}

static void
str_replace_with_string(rb_str_t *self, rb_str_t *source)
{
    if (self == source) {
	return;
    }
    str_replace_with_bytes(self, source->data.bytes, source->length_in_bytes,
	    source->encoding);
    self->flags = source->flags;
}

static void
str_append_uchar(rb_str_t *self, UChar c)
{
    assert(str_is_stored_in_uchars(self));
    const long uchar_cap = BYTES_TO_UCHARS(self->capacity_in_bytes);
    const long uchar_len = BYTES_TO_UCHARS(self->length_in_bytes);
    if (uchar_len + 1 >= uchar_cap) {
	assert(uchar_len + 1 < uchar_cap + 10);
	self->capacity_in_bytes += UCHARS_TO_BYTES(10);
	UChar *uchars = (UChar *)xrealloc(self->data.uchars,
		self->capacity_in_bytes);
	if (uchars != self->data.uchars) {
	    GC_WB(&self->data.uchars, uchars);
	}
    }
    self->data.uchars[uchar_len] = c;
    self->length_in_bytes += UCHARS_TO_BYTES(1);
}

static void
str_replace_with_uchars(rb_str_t *self, const UChar *chars, long len)
{
    assert(len >= 0);

    len = UCHARS_TO_BYTES(len);
    self->flags = 0;
    self->encoding = rb_encodings[ENCODING_UTF8];
    self->capacity_in_bytes = len;
    if (len > 0) {
	GC_WB(&self->data.uchars, xmalloc(len));
	if (chars != NULL) {
	    memcpy(self->data.uchars, chars, len);
	    self->length_in_bytes = len;
	}
	else {
	    self->length_in_bytes = 0;
	}
	str_set_stored_in_uchars(self, true);
    }
    else {
	self->data.uchars = NULL;
	self->length_in_bytes = 0;
    }
}

static void
str_replace_with_cfstring(rb_str_t *self, CFStringRef source)
{
    const long len = CFStringGetLength(source);
    UniChar *chars = NULL;
    if (len > 0) {
	chars = (UniChar *)malloc(sizeof(UniChar) * len);
	CFStringGetCharacters(source, CFRangeMake(0, len), chars);
    }
    str_replace_with_uchars(self, chars, len);
}

static void
str_replace(rb_str_t *self, VALUE arg)
{
    switch (TYPE(arg)) {
	case T_STRING:
	    if (IS_RSTR(arg)) {
		str_replace_with_string(self, RSTR(arg));
	    }
	    else {
		str_replace_with_cfstring(self, (CFStringRef)arg);
	    }
	    break;
	default:
	    str_replace(self, rb_str_to_str(arg));
	    break;
    }
}

static rb_str_t *
str_dup(rb_str_t *source)
{
    rb_str_t *destination = str_alloc(rb_cRubyString);
    str_replace_with_string(destination, source);
    return destination;
}

static rb_str_t *
str_new_from_cfstring(CFStringRef source)
{
    rb_str_t *destination = str_alloc(rb_cRubyString);
    str_replace_with_cfstring(destination, source);
    return destination;
}

static void
str_make_data_binary(rb_str_t *self)
{
    if (!str_is_stored_in_uchars(self) || NATIVE_UTF16_ENC(self->encoding)) {
	// nothing to do
	return;
    }

    if (NON_NATIVE_UTF16_ENC(self->encoding)) {
	// Doing the conversion ourself is faster, and anyway ICU's converter
	// does not like non-paired surrogates.
	str_invert_byte_order(self);
	return;
    }

    self->encoding->methods.make_data_binary(self);
}

static bool
str_try_making_data_uchars(rb_str_t *self)
{
    if (str_is_stored_in_uchars(self)) {
	return true;
    }
    else if (NON_NATIVE_UTF16_ENC(self->encoding)) {
	str_invert_byte_order(self);
	return true;
    }
    else if (BINARY_ENC(self->encoding)) {
	// you can't convert binary to anything
	return false;
    }
    else if (self->length_in_bytes == 0) {
	// for empty strings, nothing to convert
	str_set_stored_in_uchars(self, true);
	return true;
    }
    else if (str_known_to_have_an_invalid_encoding(self)) {
	return false;
    }

    return self->encoding->methods.try_making_data_uchars(self);
}

static void
str_make_same_format(rb_str_t *str1, rb_str_t *str2)
{
    if (str_is_stored_in_uchars(str1) != str_is_stored_in_uchars(str2)) {
	if (str_is_stored_in_uchars(str1)) {
	    if (!str_try_making_data_uchars(str2)) {
		str_make_data_binary(str1);
	    }
	}
	else {
	    str_make_data_binary(str2);
	}
    }
}

static long
str_length(rb_str_t *self, bool ucs2_mode)
{
    if (self->length_in_bytes == 0) {
	return 0;
    }
    if (str_is_stored_in_uchars(self)) {
	long length;
	if (ucs2_mode) {
	    length = BYTES_TO_UCHARS(self->length_in_bytes);
	}
	else {
	    // we must return the length in Unicode code points,
	    // not the number of UChars, even if the probability
	    // we have surrogates is very low
	    length = u_countChar32(self->data.uchars,
		    BYTES_TO_UCHARS(self->length_in_bytes));
	}
	if (ODD_NUMBER(self->length_in_bytes)) {
	    return length + 1;
	}
	else {
	    return length;
	}
    }
    else {
	if (self->encoding->single_byte_encoding) {
	    return self->length_in_bytes;
	}
	else if (ucs2_mode && NON_NATIVE_UTF16_ENC(self->encoding)) {
	    return div_round_up(self->length_in_bytes, 2);
	}
	else {
	    return self->encoding->methods.length(self, ucs2_mode);
	}
    }
}

static UChar
str_get_uchar(rb_str_t *self, long pos, bool ucs2_mode)
{
    assert(pos >= 0 && pos < str_length(self, ucs2_mode));
    if (str_try_making_data_uchars(self)) {
	// FIXME: Not ucs2 compliant.
	return self->data.uchars[pos];
    }
    assert(BINARY_ENC(self->encoding));
    return self->data.bytes[pos];
}

static long
str_bytesize(rb_str_t *self)
{
    if (str_is_stored_in_uchars(self)) {
	if (UTF16_ENC(self->encoding)) {
	    return self->length_in_bytes;
	}
	else {
	    return self->encoding->methods.bytesize(self);
	}
    }
    else {
	return self->length_in_bytes;
    }
}

static rb_str_t *
str_new_similar_empty_string(rb_str_t *self)
{
    rb_str_t *str = str_alloc(rb_cRubyString);
    str->encoding = self->encoding;
    str->flags = self->flags & STRING_REQUIRED_FLAGS;
    return str;
}

static rb_str_t *
str_new_copy_of_part(rb_str_t *self, long offset_in_bytes,
	long length_in_bytes)
{
    rb_str_t *str = str_alloc(rb_cRubyString);
    str->encoding = self->encoding;
    str->capacity_in_bytes = str->length_in_bytes = length_in_bytes;
    str->flags = self->flags & STRING_REQUIRED_FLAGS;
    GC_WB(&str->data.bytes, xmalloc(length_in_bytes));
    memcpy(str->data.bytes, &self->data.bytes[offset_in_bytes],
	    length_in_bytes);
    return str;
}

// you cannot cut a surrogate in an encoding that is not UTF-16
// (it's in theory possible to store the surrogate in
//  UTF-8 or UTF-32 but that would be incorrect Unicode)
NORETURN(static void
str_cannot_cut_surrogate(void))
{
    rb_raise(rb_eIndexError, "You can't cut a surrogate in two in an encoding that is not UTF-16");
}

static character_boundaries_t
str_get_character_boundaries(rb_str_t *self, long index, bool ucs2_mode)
{
    character_boundaries_t boundaries = {-1, -1};

    if (str_is_stored_in_uchars(self)) {
	if (ucs2_mode || str_known_not_to_have_any_supplementary(self)) {
	    if (index < 0) {
		index += div_round_up(self->length_in_bytes, 2);
		if (index < 0) {
		    return boundaries;
		}
	    }
	    boundaries.start_offset_in_bytes = UCHARS_TO_BYTES(index);
	    boundaries.end_offset_in_bytes = boundaries.start_offset_in_bytes
		+ 2;
	    if (!UTF16_ENC(self->encoding)) {
		long length = BYTES_TO_UCHARS(self->length_in_bytes);
		if ((index < length)
			&& U16_IS_SURROGATE(self->data.uchars[index])) {
		    if (U16_IS_SURROGATE_LEAD(self->data.uchars[index])) {
			boundaries.end_offset_in_bytes = -1;
		    }
		    else { // U16_IS_SURROGATE_TRAIL
			boundaries.start_offset_in_bytes = -1;
		    }
		}
	    }
	}
	else {
	    // we don't have the length of the string, just the number of
	    // UChars (uchars_count >= number of characters)
	    long uchars_count = BYTES_TO_UCHARS(self->length_in_bytes);
	    if ((index < -uchars_count) || (index >= uchars_count)) {
		return boundaries;
	    }
	    const UChar *uchars = self->data.uchars;
	    long offset;
	    if (index < 0) {
		// count the characters from the end
		offset = uchars_count;
		while ((offset > 0) && (index < 0)) {
		    --offset;
		    // if the next character is a paired surrogate
		    // we need to go to the start of the whole surrogate
		    if (U16_IS_TRAIL(uchars[offset]) && (offset > 0)
			    && U16_IS_LEAD(uchars[offset-1])) {
			--offset;
		    }
		    ++index;
		}
		// ended before the index got to 0
		if (index != 0) {
		    return boundaries;
		}
		assert(offset >= 0);
	    }
	    else {
		// count the characters from the start
		offset = 0;
		U16_FWD_N(uchars, offset, uchars_count, index);
		if (offset >= uchars_count) {
		    return boundaries;
		}
	    }

	    long length_in_bytes;
	    if (U16_IS_LEAD(uchars[offset]) && (offset < uchars_count - 1)
		    && (U16_IS_TRAIL(uchars[offset+1]))) {
		// if it's a lead surrogate we must also copy the trail
		// surrogate
		length_in_bytes = UCHARS_TO_BYTES(2);
	    }
	    else {
		length_in_bytes = UCHARS_TO_BYTES(1);
	    }
	    boundaries.start_offset_in_bytes = UCHARS_TO_BYTES(offset);
	    boundaries.end_offset_in_bytes = boundaries.start_offset_in_bytes
		+ length_in_bytes;
	}
    }
    else { // data in binary
	if (self->encoding->single_byte_encoding) {
	    if (index < 0) {
		index += self->length_in_bytes;
		if (index < 0) {
		    return boundaries;
		}
	    }
	    boundaries.start_offset_in_bytes = index;
	    boundaries.end_offset_in_bytes = boundaries.start_offset_in_bytes
		+ 1;
	}
	else if (UTF32_ENC(self->encoding)
		&& (!ucs2_mode
		    || str_known_not_to_have_any_supplementary(self))) {
	    if (index < 0) {
		index += div_round_up(self->length_in_bytes, 4);
		if (index < 0) {
		    return boundaries;
		}
	    }
	    boundaries.start_offset_in_bytes = index * 4;
	    boundaries.end_offset_in_bytes = boundaries.start_offset_in_bytes
		+ 4;
	}
	else if (NON_NATIVE_UTF16_ENC(self->encoding)
		&& (ucs2_mode
		    || str_known_not_to_have_any_supplementary(self))) {
	    if (index < 0) {
		index += div_round_up(self->length_in_bytes, 2);
		if (index < 0) {
		    return boundaries;
		}
	    }
	    boundaries.start_offset_in_bytes = UCHARS_TO_BYTES(index);
	    boundaries.end_offset_in_bytes = boundaries.start_offset_in_bytes
		+ 2;
	}
	else {
	    boundaries = self->encoding->methods.get_character_boundaries(self,
		    index, ucs2_mode);
	}
    }

    return boundaries;
}

static rb_str_t *
str_get_characters(rb_str_t *self, long first, long last, bool ucs2_mode)
{
    if (self->length_in_bytes == 0) {
	if (first == 0) {
	    return str_new_similar_empty_string(self);
	}
	else {
	    return NULL;
	}
    }
    if (!self->encoding->single_byte_encoding
	    && !str_is_stored_in_uchars(self)) {
	str_try_making_data_uchars(self);
    }
    character_boundaries_t first_boundaries =
	str_get_character_boundaries(self, first, ucs2_mode);
    character_boundaries_t last_boundaries =
	str_get_character_boundaries(self, last, ucs2_mode);

    if (first_boundaries.start_offset_in_bytes == -1) {
	if (last_boundaries.end_offset_in_bytes == -1) {
	    // you cannot cut a surrogate in an encoding that is not UTF-16
	    str_cannot_cut_surrogate();
	}
	else {
	    return NULL;
	}
    }
    else if (last_boundaries.end_offset_in_bytes == -1) {
	// you cannot cut a surrogate in an encoding that is not UTF-16
	str_cannot_cut_surrogate();
    }

    if (first_boundaries.start_offset_in_bytes == self->length_in_bytes) {
	return str_new_similar_empty_string(self);
    }
    else if (first_boundaries.start_offset_in_bytes > self->length_in_bytes) {
	return NULL;
    }
    if (last_boundaries.end_offset_in_bytes >= self->length_in_bytes) {
	last_boundaries.end_offset_in_bytes = self->length_in_bytes;
    }

    return str_new_copy_of_part(self, first_boundaries.start_offset_in_bytes,
	    last_boundaries.end_offset_in_bytes
	    - first_boundaries.start_offset_in_bytes);
}

static void
str_resize_bytes(rb_str_t *self, long new_capacity)
{
    if (self->capacity_in_bytes < new_capacity) {
	char *bytes = xrealloc(self->data.bytes, new_capacity);
	if (bytes != self->data.bytes) {
	    GC_WB(&self->data.bytes, bytes);
	}
	self->capacity_in_bytes = new_capacity;
    }
}

static void
str_delete(rb_str_t *self, long pos, long len, bool ucs2_mode)
{
    assert(pos >= 0 && len > 0);
    const long self_len = str_length(self, ucs2_mode);
    if (pos + len == self_len) {
	// We are deleting stuff from the end of the string. We can simply
	// change the string size here.
	if (str_is_stored_in_uchars(self)) {
	    self->length_in_bytes -= UCHARS_TO_BYTES(len);
	}
	else {
	    self->length_in_bytes -= len;
	}
    }
    else {
	assert(pos + len < self_len);
	abort(); // TODO
    }
}

static void
str_splice(rb_str_t *str, long beg, long len, rb_str_t *val, bool ucs2_mode)
{
    // TODO: str[beg..beg+len] = val
}

static void
str_concat_string(rb_str_t *self, rb_str_t *str)
{
    if (str->length_in_bytes == 0) {
	return;
    }
    if (self->length_in_bytes == 0) {
	str_replace_with_string(self, str);
	return;
    }

    str_must_have_compatible_encoding(self, str);
    str_make_same_format(self, str);

    long new_length_in_bytes = self->length_in_bytes + str->length_in_bytes;
    // TODO: we should maybe merge flags
    // (if both are ASCII-only, the concatenation is ASCII-only,
    //  though I'm not sure all the tests required are worth doing)
    str_unset_facultative_flags(self);
    str_resize_bytes(self, new_length_in_bytes);
    memcpy(self->data.bytes + self->length_in_bytes, str->data.bytes,
	    str->length_in_bytes);
    self->length_in_bytes = new_length_in_bytes;
}

static bool
str_is_equal_to_string(rb_str_t *self, rb_str_t *str)
{
    if (self == str) {
	return true;
    }

    if (self->length_in_bytes == 0) {
	if (str->length_in_bytes == 0) {
	    // both strings are empty
	    return true;
	}
	else {
	    // only self is empty
	    return false;
	}
    }
    else if (str->length_in_bytes == 0) {
	// only str is empty
	return false;
    }

    if (str_compatible_encoding(self, str) != NULL) {
	if (str_is_stored_in_uchars(self) == str_is_stored_in_uchars(str)) {
	    if (self->length_in_bytes != str->length_in_bytes) {
		return false;
	    }
	    else {
		return (memcmp(self->data.bytes, str->data.bytes,
			    self->length_in_bytes) == 0);
	    }
	}
	else { // one is in uchars and the other is in binary
	    if (!str_try_making_data_uchars(self)
		    || !str_try_making_data_uchars(str)) {
		// one is in uchars but the other one can't be converted in
		// uchars
		return false;
	    }
	    if (self->length_in_bytes != str->length_in_bytes) {
		return false;
	    }
	    else {
		return (memcmp(self->data.bytes, str->data.bytes,
			    self->length_in_bytes) == 0);
	    }
	}
    }
    else { // incompatible encodings
	return false;
    }
}

static long
str_offset_in_bytes_to_index(rb_str_t *self, long offset_in_bytes,
	bool ucs2_mode)
{
    if ((offset_in_bytes >= self->length_in_bytes) || (offset_in_bytes < 0)) {
	return -1;
    }
    if (offset_in_bytes == 0) {
	return 0;
    }

    if (str_is_stored_in_uchars(self)) {
	if (ucs2_mode || str_known_not_to_have_any_supplementary(self)) {
	    return BYTES_TO_UCHARS(offset_in_bytes);
	}
	else {
	    long length = BYTES_TO_UCHARS(self->length_in_bytes);
	    long index = 0, i = 0;
	    for (;;) {
		if (U16_IS_LEAD(self->data.uchars[i]) && (i+1 < length)
			&& U16_IS_TRAIL(self->data.uchars[i+1])) {
		    i += 2;
		}
		else {
		    ++i;
		}
		if (offset_in_bytes < i) {
		    return index;
		}
		++index;
		if (offset_in_bytes == i) {
		    return index;
		}
	    }
	}
    }
    else {
	if (self->encoding->single_byte_encoding) {
	    return offset_in_bytes;
	}
	else if (UTF32_ENC(self->encoding)
		&& (!ucs2_mode
		    || str_known_not_to_have_any_supplementary(self))) {
	    return offset_in_bytes / 4;
	}
	else if (NON_NATIVE_UTF16_ENC(self->encoding)
		&& (ucs2_mode
		    || str_known_not_to_have_any_supplementary(self))) {
	    return BYTES_TO_UCHARS(offset_in_bytes);
	}
	else {
	    return self->encoding->methods.offset_in_bytes_to_index(self,
		    offset_in_bytes, ucs2_mode);
	}
    }
}

static long
str_offset_in_bytes_for_string(rb_str_t *self, rb_str_t *searched,
	long start_offset_in_bytes)
{
    if (start_offset_in_bytes >= self->length_in_bytes) {
	return -1;
    }
    if ((self == searched) && (start_offset_in_bytes == 0)) {
	return 0;
    }
    if (searched->length_in_bytes == 0) {
	return start_offset_in_bytes;
    }
    str_must_have_compatible_encoding(self, searched);
    str_make_same_format(self, searched);
    if (searched->length_in_bytes > self->length_in_bytes) {
	return -1;
    }
    long increment;
    if (str_is_stored_in_uchars(self)) {
	increment = 2;
    }
    else {
	increment = self->encoding->min_char_size;
    }
    long max_offset_in_bytes = self->length_in_bytes
	- searched->length_in_bytes + 1;
    for (long offset_in_bytes = start_offset_in_bytes;
	    offset_in_bytes < max_offset_in_bytes;
	    offset_in_bytes += increment) {
	if (memcmp(self->data.bytes+offset_in_bytes, searched->data.bytes,
		    searched->length_in_bytes) == 0) {
	    return offset_in_bytes;
	}
    }
    return -1;
}

static long
str_index_for_string(rb_str_t *self, rb_str_t *searched, long start_index,
	bool ucs2_mode)
{
    str_must_have_compatible_encoding(self, searched);
    str_make_same_format(self, searched);

    long start_offset_in_bytes;
    if (start_index == 0) {
	start_offset_in_bytes = 0;
    }
    else {
	character_boundaries_t boundaries = str_get_character_boundaries(self,
		start_index, ucs2_mode);
	if (boundaries.start_offset_in_bytes == -1) {
	    if (boundaries.end_offset_in_bytes == -1) {
		return -1;
	    }
	    else {
		// you cannot cut a surrogate in an encoding that is not UTF-16
		str_cannot_cut_surrogate();
	    }
	}
	start_offset_in_bytes = boundaries.start_offset_in_bytes;
    }

    long offset_in_bytes = str_offset_in_bytes_for_string(RSTR(self), searched,
	    start_offset_in_bytes);

    if (offset_in_bytes == -1) {
	return -1;
    }
    return str_offset_in_bytes_to_index(RSTR(self), offset_in_bytes, ucs2_mode);
}

static bool
str_include_string(rb_str_t *self, rb_str_t *searched)
{
    return (str_offset_in_bytes_for_string(self, searched, 0) != -1);
}

static rb_str_t *
str_need_string(VALUE str)
{
    if (TYPE(str) != T_STRING) {
	str = rb_str_to_str(str);
    }
    return IS_RSTR(str)
	? (rb_str_t *)str : str_new_from_cfstring((CFStringRef)str);
}

void
rb_str_get_uchars(VALUE str, UChar **chars_p, long *chars_len_p,
	bool *need_free_p)
{
    assert(chars_p != NULL && chars_len_p != NULL && need_free_p != NULL);

    UChar *chars = NULL;
    long chars_len = 0;
    bool need_free = false;

    if (IS_RSTR(str)) {
	if (str_try_making_data_uchars(RSTR(str))) {
	    chars = RSTR(str)->data.uchars;
	    chars_len = str_length(RSTR(str), false);
	}
	else {
	    assert(BINARY_ENC(RSTR(str)->encoding));
	    chars_len = RSTR(str)->length_in_bytes;
	    if (chars_len > 0) {
		chars = (UChar *)malloc(sizeof(UChar) * chars_len);
		for (long i = 0; i < chars_len; i++) {
		    chars[i] = RSTR(str)->data.bytes[i];
		}
		need_free = true;
	    }
	}
    }
    else {
	chars_len = CFStringGetLength((CFStringRef)str);
	if (chars_len > 0) {
	    chars = (UChar *)malloc(sizeof(UChar) * chars_len);
	    CFStringGetCharacters((CFStringRef)str, CFRangeMake(0, chars_len),
		    chars);
	    need_free = true;
	}
    }

    *chars_p = chars;
    *chars_len_p = chars_len;
    *need_free_p = need_free;
}

static VALUE
str_substr(VALUE str, long beg, long len)
{
    if (len < 0) {
	return Qnil;
    }
    if (len == 0) {
	return str_new();
    }	

    const long n = str_length(RSTR(str), false);
    if (beg < 0) {
	beg += n;
    }
    if (beg > n || beg < 0) {
	return Qnil;
    }
    if (beg + len > n) {
	len = n - beg;
    }

    rb_str_t *substr = str_get_characters(RSTR(str), beg, beg + len - 1, false);
    return substr == NULL ? Qnil : (VALUE)substr;
}

static VALUE
str_trim(VALUE str)
{
    // TODO
    return str;
}

//----------------------------------------------
// Functions called by MacRuby

VALUE
mr_enc_s_is_compatible(VALUE klass, SEL sel, VALUE str1, VALUE str2)
{
    if (SPECIAL_CONST_P(str1) || SPECIAL_CONST_P(str2)) {
	return Qnil;
    }
    assert(IS_RSTR(str1)); // TODO
    assert(IS_RSTR(str2)); // TODO
    rb_encoding_t *encoding = str_compatible_encoding(RSTR(str1), RSTR(str2));
    if (encoding == NULL) {
	return Qnil;
    }
    else {
	return (VALUE)encoding;
    }
}

static VALUE
rstr_alloc(VALUE klass, SEL sel)
{
    return (VALUE)str_alloc(klass);
}

/*
 *  call-seq:
 *     String.new(str="")   => new_str
 *  
 *  Returns a new string object containing a copy of <i>str</i>.
 */

static VALUE
rstr_initialize(VALUE self, SEL sel, int argc, VALUE *argv)
{
    if (argc > 0) {
	assert(argc == 1);
	str_replace(RSTR(self), argv[0]);
    }
    return self;
}

/*
 *  call-seq:
 *     str.replace(other_str)   => str
 *  
 *  Replaces the contents and taintedness of <i>str</i> with the corresponding
 *  values in <i>other_str</i>.
 *     
 *     s = "hello"         #=> "hello"
 *     s.replace "world"   #=> "world"
 */

static VALUE
rstr_replace(VALUE self, SEL sel, VALUE arg)
{
    rstr_modify(self);
    str_replace(RSTR(self), arg);
    return self;
}

static VALUE
rstr_copy(VALUE rcv, VALUE klass)
{
    VALUE dup = rstr_alloc(klass, 0);
    rstr_replace(dup, 0, rcv);
    return dup;
}

static VALUE
rstr_dup(VALUE str, SEL sel)
{
    VALUE klass = CLASS_OF(str);
    while (RCLASS_SINGLETON(klass)) {
	klass = RCLASS_SUPER(klass);
    }
    assert(rb_klass_is_rstr(klass));

    VALUE dup = rstr_copy(str, klass);

    if (OBJ_TAINTED(str)) {
	OBJ_TAINT(dup);
    }
    if (OBJ_UNTRUSTED(str)) {
	OBJ_UNTRUST(dup);
    }
    return dup;
}

static VALUE
rstr_clone(VALUE str, SEL sel)
{
    VALUE clone = rstr_copy(str, CLASS_OF(str));

    if (OBJ_TAINTED(str)) {
	OBJ_TAINT(clone);
    }
    if (OBJ_UNTRUSTED(str)) {
	OBJ_UNTRUST(clone);
    }
    if (OBJ_FROZEN(str)) {
	OBJ_FREEZE(clone);
    }
    return clone;
}

/*
 *  call-seq:
 *     string.clear    ->  string
 *
 *  Makes string empty.
 *
 *     a = "abcde"
 *     a.clear    #=> ""
 */

static VALUE
rstr_clear(VALUE self, SEL sel)
{
    rstr_modify(self);
    RSTR(self)->length_in_bytes = 0;
    return self;
}

static VALUE
rstr_chars_count(VALUE self, SEL sel)
{
    return INT2NUM(str_length(RSTR(self), false));
}

/*
 *  call-seq:
 *     str.length   => integer
 *     str.size     => integer
 *  
 *  Returns the character length of <i>str</i>.
 */

static VALUE
rstr_length(VALUE self, SEL sel)
{
    return INT2NUM(str_length(RSTR(self), true));
}

/*
 *  call-seq:
 *     str.bytesize  => integer
 *  
 *  Returns the length of <i>str</i> in bytes.
 */

static VALUE
rstr_bytesize(VALUE self, SEL sel)
{
    return INT2NUM(str_bytesize(RSTR(self)));
}

static VALUE
rstr_encoding(VALUE self, SEL sel)
{
    return (VALUE)RSTR(self)->encoding;
}

/*
 *  call-seq:
 *     str.getbyte(index)          => 0 .. 255
 *
 *  returns the <i>index</i>th byte as an integer.
 */

static VALUE
rstr_getbyte(VALUE self, SEL sel, VALUE index)
{
    unsigned char c = 0;
    long idx = NUM2LONG(index);

    if (str_is_stored_in_uchars(RSTR(self))
	    && NATIVE_UTF16_ENC(RSTR(self)->encoding)) {
	if (idx < 0) {
	    idx += RSTR(self)->length_in_bytes;
	    if (idx < 0) {
		return Qnil;
	    }
	}
	if (idx >= RSTR(self)->length_in_bytes) {
	    return Qnil;
	}
	if (NATIVE_UTF16_ENC(RSTR(self)->encoding)) {
	    c = RSTR(self)->data.bytes[idx];
	}
	else { // non native byte-order UTF-16
	    if ((idx & 1) == 0) { // even
		c = RSTR(self)->data.bytes[idx+1];
	    }
	    else { // odd
		c = RSTR(self)->data.bytes[idx-1];
	    }
	}
    }
    else {
	// work with a binary string
	// (UTF-16 strings could be converted to their binary form
	//  on the fly but that would just add complexity)
	str_make_data_binary(RSTR(self));

	if (idx < 0) {
	    idx += RSTR(self)->length_in_bytes;
	    if (idx < 0) {
		return Qnil;
	    }
	}
	if (idx >= RSTR(self)->length_in_bytes) {
	    return Qnil;
	}
	c = RSTR(self)->data.bytes[idx];
    }

    return INT2FIX(c); 
}

/*
 *  call-seq:
 *     str.setbyte(index, int) => int
 *
 *  modifies the <i>index</i>th byte as <i>int</i>.
 */

static VALUE
rstr_setbyte(VALUE self, SEL sel, VALUE index, VALUE value)
{
    rstr_modify(self);
    str_make_data_binary(RSTR(self));
    if ((index < -RSTR(self)->length_in_bytes)
	    || (index >= RSTR(self)->length_in_bytes)) {
	rb_raise(rb_eIndexError, "index %ld out of string", index);
    }
    if (index < 0) {
	index += RSTR(self)->length_in_bytes;
    }
    RSTR(self)->data.bytes[index] = value;
    return value;
}

/*
 *  call-seq:
 *     str.force_encoding(encoding)   => str
 *
 *  Changes the encoding to +encoding+ and returns self.
 */

static VALUE
rstr_force_encoding(VALUE self, SEL sel, VALUE encoding)
{
    rstr_modify(self);
    rb_encoding_t *enc = rb_to_encoding(encoding);
    if (enc != RSTR(self)->encoding) {
	str_make_data_binary(RSTR(self));
	if (NATIVE_UTF16_ENC(RSTR(self)->encoding)) {
	    str_set_stored_in_uchars(RSTR(self), false);
	}
	RSTR(self)->encoding = enc;
	str_unset_facultative_flags(RSTR(self));
	if (NATIVE_UTF16_ENC(RSTR(self)->encoding)) {
	    str_set_stored_in_uchars(RSTR(self), true);
	}
    }
    return self;
}

/*
 *  call-seq:
 *     str.valid_encoding?  => true or false
 *  
 *  Returns true for a string which encoded correctly.
 *
 *    "\xc2\xa1".force_encoding("UTF-8").valid_encoding? => true
 *    "\xc2".force_encoding("UTF-8").valid_encoding? => false
 *    "\x80".force_encoding("UTF-8").valid_encoding? => false
 */

static VALUE
rstr_is_valid_encoding(VALUE self, SEL sel)
{
    return str_is_valid_encoding(RSTR(self)) ? Qtrue : Qfalse;
}

/*
 *  call-seq:
 *     str.ascii_only?  => true or false
 *  
 *  Returns true for a string which has only ASCII characters.
 *
 *    "abc".force_encoding("UTF-8").ascii_only? => true
 *    "abc\u{6666}".force_encoding("UTF-8").ascii_only? => false
 */

static VALUE
rstr_is_ascii_only(VALUE self, SEL sel)
{
    return str_is_ruby_ascii_only(RSTR(self)) ? Qtrue : Qfalse;
}

/*
 *  call-seq:
 *     str[fixnum]                 => new_str or nil
 *     str[fixnum, fixnum]         => new_str or nil
 *     str[range]                  => new_str or nil
 *     str[regexp]                 => new_str or nil
 *     str[regexp, fixnum]         => new_str or nil
 *     str[other_str]              => new_str or nil
 *     str.slice(fixnum)           => new_str or nil
 *     str.slice(fixnum, fixnum)   => new_str or nil
 *     str.slice(range)            => new_str or nil
 *     str.slice(regexp)           => new_str or nil
 *     str.slice(regexp, fixnum)   => new_str or nil
 *     str.slice(other_str)        => new_str or nil
 *  
 *  Element Reference---If passed a single <code>Fixnum</code>, returns a
 *  substring of one character at that position. If passed two <code>Fixnum</code>
 *  objects, returns a substring starting at the offset given by the first, and
 *  a length given by the second. If given a range, a substring containing
 *  characters at offsets given by the range is returned. In all three cases, if
 *  an offset is negative, it is counted from the end of <i>str</i>. Returns
 *  <code>nil</code> if the initial offset falls outside the string, the length
 *  is negative, or the beginning of the range is greater than the end.
 *     
 *  If a <code>Regexp</code> is supplied, the matching portion of <i>str</i> is
 *  returned. If a numeric parameter follows the regular expression, that
 *  component of the <code>MatchData</code> is returned instead. If a
 *  <code>String</code> is given, that string is returned if it occurs in
 *  <i>str</i>. In both cases, <code>nil</code> is returned if there is no
 *  match.
 *     
 *     a = "hello there"
 *     a[1]                   #=> "e"
 *     a[1,3]                 #=> "ell"
 *     a[1..3]                #=> "ell"
 *     a[-3,2]                #=> "er"
 *     a[-4..-2]              #=> "her"
 *     a[12..-1]              #=> nil
 *     a[-2..-4]              #=> ""
 *     a[/[aeiou](.)\1/]      #=> "ell"
 *     a[/[aeiou](.)\1/, 0]   #=> "ell"
 *     a[/[aeiou](.)\1/, 1]   #=> "l"
 *     a[/[aeiou](.)\1/, 2]   #=> nil
 *     a["lo"]                #=> "lo"
 *     a["bye"]               #=> nil
 */

static VALUE
rb_str_subpat(VALUE str, VALUE re, int nth)
{
    if (rb_reg_search(re, str, 0, 0) >= 0) {
	return rb_reg_nth_match(nth, rb_backref_get());
    }
    return Qnil;
}

static VALUE
rstr_aref(VALUE str, SEL sel, int argc, VALUE *argv)
{
    if (argc == 2) {
	if (TYPE(argv[0]) == T_REGEXP) {
	    return rb_str_subpat(str, argv[0], NUM2INT(argv[1]));
	}
	return str_substr(str, NUM2LONG(argv[0]), NUM2LONG(argv[1]));
    }

    if (argc != 1) {
	rb_raise(rb_eArgError, "wrong number of arguments (%d for 1)", argc);
    }

    VALUE indx = argv[0];
    switch (TYPE(indx)) {
	case T_FIXNUM:
	    str = str_substr(str, FIX2LONG(indx), 1);
	    if (!NIL_P(str) && str_length(RSTR(str), true) == 0) {
		return Qnil;
	    }
	    return str;

	case T_REGEXP:
	    return rb_str_subpat(str, indx, 0);

	case T_STRING:
	    {
		if (IS_RSTR(indx)) {
		    rb_str_t *searched = RSTR(indx);
		    if (str_include_string(RSTR(str), searched)) {
			return (VALUE)str_dup(searched);
		    }
		}
		else {
		    rb_str_t *searched =
			str_new_from_cfstring((CFStringRef)indx);
		    if (str_include_string(RSTR(str), searched)) {
			// no need to duplicate the string as we just
			// created it
			return (VALUE)searched;
		    }
		}
		return Qnil;
	    }

	default:
	    {
		VALUE rb_start = 0, rb_end = 0;
		int exclude_end = false;
		if (rb_range_values(indx, &rb_start, &rb_end,
			    &exclude_end)) {
		    long start = NUM2LONG(rb_start);
		    long end = NUM2LONG(rb_end);
		    if (exclude_end) {
			--end;
		    }
		    return str_substr(str, start, end - start + 1);
		}
		else {
		    str = str_substr(str, NUM2LONG(indx), 1);
		    if (!NIL_P(str) && str_length(RSTR(str), true) == 0) {
			return Qnil;
		    }
		    return str;
		}
	    }
    }
}

/*
 *  call-seq:
 *     str.index(substring [, offset])   => fixnum or nil
 *     str.index(fixnum [, offset])      => fixnum or nil
 *     str.index(regexp [, offset])      => fixnum or nil
 *  
 *  Returns the index of the first occurrence of the given <i>substring</i>,
 *  character (<i>fixnum</i>), or pattern (<i>regexp</i>) in <i>str</i>. Returns
 *  <code>nil</code> if not found. If the second parameter is present, it
 *  specifies the position in the string to begin the search.
 *     
 *     "hello".index('e')             #=> 1
 *     "hello".index('lo')            #=> 3
 *     "hello".index('a')             #=> nil
 *     "hello".index(?e)              #=> 1
 *     "hello".index(101)             #=> 1
 *     "hello".index(/[aeiou]/, -3)   #=> 4
 */

static VALUE
rstr_index(VALUE self, SEL sel, int argc, VALUE *argv)
{
    VALUE sub, initpos;
    long pos;

    if (rb_scan_args(argc, argv, "11", &sub, &initpos) == 2) {
	pos = NUM2LONG(initpos);
    }
    else {
	pos = 0;
    }

    if (pos < 0) {
	pos += str_length(RSTR(self), true);
	if (pos < 0) {
	    if (TYPE(sub) == T_REGEXP) {
		rb_backref_set(Qnil);
	    }
	    return Qnil;
	}
    }

    switch (TYPE(sub)) {
	case T_REGEXP:
	    pos = rb_reg_adjust_startpos(sub, self, pos, false);
	    pos = rb_reg_search(sub, self, pos, false);
	    break;

	default: 
	    {
		VALUE tmp = rb_check_string_type(sub);
		if (NIL_P(tmp)) {
		    rb_raise(rb_eTypeError, "type mismatch: %s given",
			    rb_obj_classname(sub));
		}
		sub = tmp;
	    }
	    /* fall through */
	case T_STRING:
	    {
		rb_str_t *substr = str_need_string(sub);
		pos = str_index_for_string(RSTR(self), substr, pos, true);
	    }
	    break;
    }

    if (pos == -1) {
	return Qnil;
    }
    return LONG2NUM(pos);
}

static VALUE
rstr_getchar(VALUE self, SEL sel, VALUE index)
{
    const long idx = FIX2LONG(index);
    return str_substr(self, idx, 1);
}

/*
 *  call-seq:
 *     str + other_str   => new_str
 *  
 *  Concatenation---Returns a new <code>String</code> containing
 *  <i>other_str</i> concatenated to <i>str</i>.
 *     
 *     "Hello from " + self.to_s   #=> "Hello from main"
 */

static VALUE
rstr_plus(VALUE self, SEL sel, VALUE other)
{
    rb_str_t *newstr = str_dup(RSTR(self));
    str_concat_string(newstr, str_need_string(other));
    return (VALUE)newstr;
}

/*
 *  call-seq:
 *     str << fixnum        => str
 *     str.concat(fixnum)   => str
 *     str << obj           => str
 *     str.concat(obj)      => str
 *  
 *  Append---Concatenates the given object to <i>str</i>. If the object is a
 *  <code>Fixnum</code>, it is considered as a codepoint, and is converted
 *  to a character before concatenation.
 *     
 *     a = "hello "
 *     a << "world"   #=> "hello world"
 *     a.concat(33)   #=> "hello world!"
 */

static VALUE
rstr_concat(VALUE self, SEL sel, VALUE other)
{
    rstr_modify(self);

    long codepoint = 0;
    switch (TYPE(other)) {
	case T_FIXNUM:
	    codepoint = FIX2LONG(other);
	    break;

	case T_BIGNUM:
	    codepoint = rb_big2ulong(other);
	    break;
	    
	default:
	    str_concat_string(RSTR(self), str_need_string(other));
	    return self;
    }

    // TODO: handle codepoint

    return self;
}

/*
 *  call-seq:
 *     str == obj   => true or false
 *  
 *  Equality---If <i>obj</i> is not a <code>String</code>, returns
 *  <code>false</code>. Otherwise, returns <code>true</code> if <i>str</i>
 *  <code><=></code> <i>obj</i> returns zero.
 */

static VALUE
rstr_equal(VALUE self, SEL sel, VALUE other)
{
    if (self == other) {
	return Qtrue;
    }

    if (TYPE(other) != T_STRING) {
	if (!rb_respond_to(other, rb_intern("to_str"))) {
	    return Qfalse;
	}
	return rb_equal(other, self);
    }

    rb_str_t *str;
    if (IS_RSTR(other)) {
	str = RSTR(other);
    }
    else {
	str = str_new_from_cfstring((CFStringRef)other);
    }
    return str_is_equal_to_string(RSTR(self), str) ? Qtrue : Qfalse;
}

/*
 *  call-seq:
 *     str.include? other_str   => true or false
 *     str.include? fixnum      => true or false
 *  
 *  Returns <code>true</code> if <i>str</i> contains the given string or
 *  character.
 *     
 *     "hello".include? "lo"   #=> true
 *     "hello".include? "ol"   #=> false
 *     "hello".include? ?h     #=> true
 */

static VALUE
rstr_includes(VALUE self, SEL sel, VALUE searched)
{
    return str_include_string(RSTR(self), str_need_string(searched))
	? Qtrue : Qfalse;
}

static VALUE
rstr_is_stored_in_uchars(VALUE self, SEL sel)
{
    return str_is_stored_in_uchars(RSTR(self)) ? Qtrue : Qfalse;
}

/*
 *  call-seq:
 *     str.to_s     => str
 *     str.to_str   => str
 *  
 *  Returns the receiver.
 */

static VALUE
rstr_to_s(VALUE self, SEL sel)
{
    if (CLASS_OF(self) != rb_cRubyString) {
	VALUE dup = (VALUE)str_dup(RSTR(self));
	if (OBJ_TAINTED(self)) {
	    OBJ_TAINT(dup);
	}
	return dup;
    }
    return self;
}

/*
 *  call-seq:
 *     str.intern   => symbol
 *     str.to_sym   => symbol
 *  
 *  Returns the <code>Symbol</code> corresponding to <i>str</i>, creating the
 *  symbol if it did not previously exist. See <code>Symbol#id2name</code>.
 *     
 *     "Koala".intern         #=> :Koala
 *     s = 'cat'.to_sym       #=> :cat
 *     s == :cat              #=> true
 *     s = '@cat'.to_sym      #=> :@cat
 *     s == :@cat             #=> true
 *
 *  This can also be used to create symbols that cannot be represented using the
 *  <code>:xxx</code> notation.
 *     
 *     'cat and dog'.to_sym   #=> :"cat and dog"
 */

static VALUE
rstr_intern(VALUE self, SEL sel)
{
    if (OBJ_TAINTED(self) && rb_safe_level() >= 1) {
	rb_raise(rb_eSecurityError, "Insecure: can't intern tainted string");
    }
    str_make_data_binary(RSTR(self));
    return ID2SYM(rb_intern(RSTR(self)->data.bytes));
}

/*
 * call-seq:
 *   str.inspect   => string
 *
 * Returns a printable version of _str_, surrounded by quote marks,
 * with special characters escaped.
 *
 *    str = "hello"
 *    str[3] = "\b"
 *    str.inspect       #=> "\"hel\\bo\""
 */

static void
inspect_append(VALUE result, UChar c, bool escape)
{
    if (escape) {
	str_append_uchar(RSTR(result), '\\');
    }
    str_append_uchar(RSTR(result), c);
}

static VALUE
str_inspect(VALUE str, bool dump)
{
    const bool uchars = str_is_stored_in_uchars(RSTR(str));
    const long len = uchars
	? str_length(RSTR(str), true) : RSTR(str)->length_in_bytes;

    if (len == 0) {
	return rb_str_new2("\"\"");
    }

    // Allocate an UTF-8 string with a good initial capacity.
    // Binary strings will likely have most bytes escaped.
    const long result_init_len =
	BINARY_ENC(RSTR(str)->encoding) ? (len * 5) + 2 : len + 2;
    VALUE result = rb_unicode_str_new(NULL, result_init_len);

#define GET_UCHAR(pos) \
    ((uchars \
      ? RSTR(str)->data.uchars[pos] : (UChar)RSTR(str)->data.bytes[pos]))

    inspect_append(result, '"', false);
    for (long i = 0; i < len; i++) {
	const UChar c = GET_UCHAR(i);

	if (iswprint(c)) {
	    if (c == '"' || c == '\\') {
		inspect_append(result, c, true);
	    }
	    else if (dump && c == '#' && i + 1 < len) {
		const UChar c2 = GET_UCHAR(i + 1);
		const bool need_escape = c2 == '$' || c2 == '@' || c2 == '{';
		inspect_append(result, c, need_escape);
	    }
	    else {
		inspect_append(result, c, false);
	    }
	}
	else if (c == '\n') {
	    inspect_append(result, 'n', true);
	} 
	else if (c == '\r') {
	    inspect_append(result, 'r', true);
	} 
	else if (c == '\t') {
	    inspect_append(result, 't', true);
	} 
	else if (c == '\f') {
	    inspect_append(result, 'f', true);
	}
	else if (c == '\013') {
	    inspect_append(result, 'v', true);
	}
	else if (c == '\010') {
	    inspect_append(result, 'b', true);
	}
	else if (c == '\007') {
	    inspect_append(result, 'a', true);
	}
	else if (c == 033) {
	    inspect_append(result, 'e', true);
	}
	else {
	    char buf[10];
	    snprintf(buf, sizeof buf, "\\x%02X", c);
	    char *p = buf;
	    while (*p != '\0') {
		inspect_append(result, *p, false);
		p++;
	    }
	}
    }
    inspect_append(result, '"', false);
   
#undef GET_UCHAR

    return result; 
}

static VALUE
rstr_inspect(VALUE self, SEL sel)
{
    return str_inspect(self, false);
}

/*
 *  call-seq:
 *     str.dump   => new_str
 *  
 *  Produces a version of <i>str</i> with all nonprinting characters replaced by
 *  <code>\nnn</code> notation and all special characters escaped.
 */

static VALUE
rstr_dump(VALUE self, SEL sel)
{
    return str_inspect(self, true);
}

/*
 *  call-seq:
 *     str.match(pattern)   => matchdata or nil
 *  
 *  Converts <i>pattern</i> to a <code>Regexp</code> (if it isn't already one),
 *  then invokes its <code>match</code> method on <i>str</i>.  If the second
 *  parameter is present, it specifies the position in the string to begin the
 *  search.
 *     
 *     'hello'.match('(.)\1')      #=> #<MatchData "ll" 1:"l">
 *     'hello'.match('(.)\1')[0]   #=> "ll"
 *     'hello'.match(/(.)\1/)[0]   #=> "ll"
 *     'hello'.match('xx')         #=> nil
 *     
 *  If a block is given, invoke the block with MatchData if match succeed, so
 *  that you can write
 *     
 *     str.match(pat) {|m| ...}
 *     
 *  instead of
 *      
 *     if m = str.match(pat)
 *       ...
 *     end
 *      
 *  The return value is a value from block execution in this case.
 */

static VALUE
get_pat(VALUE pat, bool quote)
{
    switch (TYPE(pat)) {
	case T_REGEXP:
	    return pat;

	case T_STRING:
	    break;

	default:
	    {
		VALUE val = rb_check_string_type(pat);
		if (NIL_P(val)) {
		    Check_Type(pat, T_REGEXP);
		}
		pat = val;
	    }
    }

    if (quote) {
	pat = rb_reg_quote(pat);
    }
    return rb_reg_regcomp(pat);
}

static VALUE
rstr_match2(VALUE self, SEL sel, int argc, VALUE *argv)
{
    if (argc < 1) {
	rb_raise(rb_eArgError, "wrong number of arguments (%d for 1)", argc);
    }
    VALUE re = get_pat(argv[0], false);
    argv[0] = self;
    VALUE result = regexp_match2(re, 0, argc, argv);
    if (!NIL_P(result) && rb_block_given_p()) {
	return rb_yield(result);
    }
    return result;
}

/*
 *  call-seq:
 *     str =~ obj   => fixnum or nil
 *  
 *  Match---If <i>obj</i> is a <code>Regexp</code>, use it as a pattern to match
 *  against <i>str</i>,and returns the position the match starts, or 
 *  <code>nil</code> if there is no match. Otherwise, invokes
 *  <i>obj.=~</i>, passing <i>str</i> as an argument. The default
 *  <code>=~</code> in <code>Object</code> returns <code>false</code>.
 *     
 *     "cat o' 9 tails" =~ /\d/   #=> 7
 *     "cat o' 9 tails" =~ 9      #=> nil
 */

static VALUE
rstr_match(VALUE self, SEL sel, VALUE other)
{
    switch (TYPE(other)) {
	case T_STRING:
	    rb_raise(rb_eTypeError, "type mismatch: String given");

	case T_REGEXP:
	    return regexp_match(other, 0, self);

	default:
	    return rb_vm_call(other, selEqTilde, 1, &self, false);
    }
}

/*
 *  call-seq:
 *     str.scan(pattern)                         => array
 *     str.scan(pattern) {|match, ...| block }   => str
 *  
 *  Both forms iterate through <i>str</i>, matching the pattern (which may be a
 *  <code>Regexp</code> or a <code>String</code>). For each match, a result is
 *  generated and either added to the result array or passed to the block. If
 *  the pattern contains no groups, each individual result consists of the
 *  matched string, <code>$&</code>.  If the pattern contains groups, each
 *  individual result is itself an array containing one entry per group.
 *     
 *     a = "cruel world"
 *     a.scan(/\w+/)        #=> ["cruel", "world"]
 *     a.scan(/.../)        #=> ["cru", "el ", "wor"]
 *     a.scan(/(...)/)      #=> [["cru"], ["el "], ["wor"]]
 *     a.scan(/(..)(..)/)   #=> [["cr", "ue"], ["l ", "wo"]]
 *     
 *  And the block form:
 *     
 *     a.scan(/\w+/) {|w| print "<<#{w}>> " }
 *     print "\n"
 *     a.scan(/(.)(.)/) {|x,y| print y, x }
 *     print "\n"
 *     
 *  <em>produces:</em>
 *     
 *     <<cruel>> <<world>>
 *     rceu lowlr
 */

static VALUE
rstr_scan(VALUE self, SEL sel, VALUE pat)
{
    const bool block_given = rb_block_given_p();

    pat = get_pat(pat, true);
    long start = 0;

    VALUE ary = 0;
    if (!block_given) {
	ary = rb_ary_new();
    }

    while (rb_reg_search(pat, self, start, false) >= 0) {
	VALUE match = rb_backref_get();

	int count = 0;
	rb_match_result_t *results = rb_reg_match_results(match, &count);
	assert(count > 0);
	start = results[0].end;

	VALUE scan_result;
	if (count == 1) {
	    scan_result = rb_reg_nth_match(0, match);
	}
	else {
	    scan_result = rb_ary_new2(count);
	    for (int i = 1; i < count; i++) {
		rb_ary_push(scan_result, rb_reg_nth_match(i, match));
	    }
	}

	if (block_given) {
	    rb_yield(scan_result);
	    rb_backref_set(match);
	    RETURN_IF_BROKEN();
	}
	else {
	    rb_ary_push(ary, scan_result);
	}
    }

    return block_given ? self : ary;
}

/*
 *  call-seq:
 *     str.split(pattern=$;, [limit])   => anArray
 *  
 *  Divides <i>str</i> into substrings based on a delimiter, returning an array
 *  of these substrings.
 *     
 *  If <i>pattern</i> is a <code>String</code>, then its contents are used as
 *  the delimiter when splitting <i>str</i>. If <i>pattern</i> is a single
 *  space, <i>str</i> is split on whitespace, with leading whitespace and runs
 *  of contiguous whitespace characters ignored.
 *     
 *  If <i>pattern</i> is a <code>Regexp</code>, <i>str</i> is divided where the
 *  pattern matches. Whenever the pattern matches a zero-length string,
 *  <i>str</i> is split into individual characters. If <i>pattern</i> contains
 *  groups, the respective matches will be returned in the array as well.
 *     
 *  If <i>pattern</i> is omitted, the value of <code>$;</code> is used.  If
 *  <code>$;</code> is <code>nil</code> (which is the default), <i>str</i> is
 *  split on whitespace as if ` ' were specified.
 *     
 *  If the <i>limit</i> parameter is omitted, trailing null fields are
 *  suppressed. If <i>limit</i> is a positive number, at most that number of
 *  fields will be returned (if <i>limit</i> is <code>1</code>, the entire
 *  string is returned as the only entry in an array). If negative, there is no
 *  limit to the number of fields returned, and trailing null fields are not
 *  suppressed.
 *     
 *     " now's  the time".split        #=> ["now's", "the", "time"]
 *     " now's  the time".split(' ')   #=> ["now's", "the", "time"]
 *     " now's  the time".split(/ /)   #=> ["", "now's", "", "the", "time"]
 *     "1, 2.34,56, 7".split(%r{,\s*}) #=> ["1", "2.34", "56", "7"]
 *     "hello".split(//)               #=> ["h", "e", "l", "l", "o"]
 *     "hello".split(//, 3)            #=> ["h", "e", "llo"]
 *     "hi mom".split(%r{\s*})         #=> ["h", "i", "m", "o", "m"]
 *     
 *     "mellow yellow".split("ello")   #=> ["m", "w y", "w"]
 *     "1,2,,3,4,,".split(',')         #=> ["1", "2", "", "3", "4"]
 *     "1,2,,3,4,,".split(',', 4)      #=> ["1", "2", "", "3,4,,"]
 *     "1,2,,3,4,,".split(',', -4)     #=> ["1", "2", "", "3", "4", "", ""]
 */

static VALUE
rstr_split(VALUE str, SEL sel, int argc, VALUE *argv)
{
    const long len = str_length(RSTR(str), false);
    int lim = 0;

    VALUE spat, limit;
    if (rb_scan_args(argc, argv, "02", &spat, &limit) == 2) {
	lim = NUM2INT(limit);
	if (lim <= 0) {
	    limit = Qnil;
	}
	else if (lim == 1) {
	    if (len == 0) {
		return rb_ary_new2(0);
	    }
	    return rb_ary_new3(1, str);
	}
    }

    VALUE result = rb_ary_new();
    bool awk_split = false, spat_string = false;
    long spat_len = 0;
    if (NIL_P(spat)) {
	if (!NIL_P(rb_fs)) {
	    spat = rb_fs;
	    goto fs_set;
	}
	awk_split = true;
    }
    else {
fs_set:
	if (TYPE(spat) == T_STRING) {
	    spat_string = true;
	    spat_len = rb_str_chars_len(spat);
	    if (spat_len == 1 && rb_str_get_uchar(spat, 0) == ' ') {
		awk_split = true;
	    }
	}
	else {
	    spat = get_pat(spat, true);
	}
    }

    long beg = 0;
    if (awk_split || spat_string) {
	if (spat != Qnil) {
	    if (spat_len == 0) {
		do {
		    VALUE substr = str_substr(str, beg, 1);
		    rb_ary_push(result, substr);
		    beg++;
		    if (beg >= len) {
			break;
		    }
		}
		while (limit == Qnil || --lim > 1);
	    }
	    else {
		rb_str_t *spat_str = str_need_string(spat);
		do {
		    const long pos = str_index_for_string(RSTR(str), spat_str,
			    beg, false);
		    if (pos == -1) {
			break;
		    }
		    VALUE substr = str_substr(str, beg, pos - beg);
		    if (!awk_split || rb_str_chars_len(str_trim(substr)) > 0) {
			rb_ary_push(result, substr);
		    }
		    beg = pos + 1;
		}
		while (limit == Qnil || --lim > 1);
	    }
	}
	else {
	    abort(); // TODO
	}
    }
    else {
	long start = beg;
	bool last_null = false;
	do {
	    const long pos = rb_reg_search(spat, str, beg, false);
	    if (pos < 0) {
		break;
	    }
	    VALUE match = rb_backref_get();

	    int count = 0;
	    rb_match_result_t *results = rb_reg_match_results(match, &count);
	    assert(count > 0);

	    if (beg == pos && results[0].beg == results[0].end) {
		if (last_null) {
		    rb_ary_push(result, str_substr(str, beg, 1));
		    beg = start;
		}
		else {
		    start++;
		    last_null = true;
		    continue;
		}
	    }
	    else {
		rb_ary_push(result, str_substr(str, beg, pos - beg));
		beg = results[0].end;
	    }
	    last_null = false;

	    for (int i = 1; i < count; i++) {
		rb_ary_push(result, rb_reg_nth_match(i, match));
	    }
	}
	while (limit == Qnil || --lim > 1);
    }

    if (len > 0 && (!NIL_P(limit) || len > beg || lim < 0)) {
	VALUE tmp;
	if (len == beg) {
	    tmp = rb_str_new(NULL, 0);
	}
	else {
	    tmp = rb_str_subseq(str, beg, len - beg);
	}
	rb_ary_push(result, tmp);
    }

    if (NIL_P(limit) && lim == 0) {
	while (true) {
	    const long n = RARRAY_LEN(result);
	    if (n > 0 && rb_str_chars_len(RARRAY_AT(result, n - 1)) == 0) {
		rb_ary_pop(result);
	    }
	    else {
		break;
	    }
	}
    }

    return result;
}

/*
 *  call-seq:
 *     str.to_i(base=10)   => integer
 *  
 *  Returns the result of interpreting leading characters in <i>str</i> as an
 *  integer base <i>base</i> (between 2 and 36). Extraneous characters past the
 *  end of a valid number are ignored. If there is not a valid number at the
 *  start of <i>str</i>, <code>0</code> is returned. This method never raises an
 *  exception.
 *     
 *     "12345".to_i             #=> 12345
 *     "99 red balloons".to_i   #=> 99
 *     "0a".to_i                #=> 0
 *     "0a".to_i(16)            #=> 10
 *     "hello".to_i             #=> 0
 *     "1100101".to_i(2)        #=> 101
 *     "1100101".to_i(8)        #=> 294977
 *     "1100101".to_i(10)       #=> 1100101
 *     "1100101".to_i(16)       #=> 17826049
 */

static VALUE
rstr_to_i(VALUE str, SEL sel, int argc, VALUE *argv)
{
    int base = 10;

    if (argc > 0) {
	VALUE b;
	rb_scan_args(argc, argv, "01", &b);

	base = NUM2INT(b);
	if (base < 0) {
	    rb_raise(rb_eArgError, "invalid radix %d", base);
	}
    }

    return rb_str_to_inum(str, base, Qfalse);
}

/*
 *  call-seq:
 *     str.hex   => integer
 *  
 *  Treats leading characters from <i>str</i> as a string of hexadecimal digits
 *  (with an optional sign and an optional <code>0x</code>) and returns the
 *  corresponding number. Zero is returned on error.
 *     
 *     "0x0a".hex     #=> 10
 *     "-1234".hex    #=> -4660
 *     "0".hex        #=> 0
 *     "wombat".hex   #=> 0
 */

static VALUE
rstr_hex(VALUE str, SEL sel)
{
    return rb_str_to_inum(str, 16, Qfalse);
}

/*
 *  call-seq:
 *     str.oct   => integer
 *  
 *  Treats leading characters of <i>str</i> as a string of octal digits (with an
 *  optional sign) and returns the corresponding number.  Returns 0 if the
 *  conversion fails.
 *     
 *     "123".oct       #=> 83
 *     "-377".oct      #=> -255
 *     "bad".oct       #=> 0
 *     "0377bad".oct   #=> 255
 */

static VALUE
rstr_oct(VALUE str, SEL sel)
{
    return rb_str_to_inum(str, -8, Qfalse);
}

/*
 *  call-seq:
 *     str.chomp!(separator=$/)   => str or nil
 *  
 *  Modifies <i>str</i> in place as described for <code>String#chomp</code>,
 *  returning <i>str</i>, or <code>nil</code> if no modifications were made.
 */

static VALUE
rstr_chomp_bang(VALUE str, SEL sel, int argc, VALUE *argv)
{
    VALUE rs;
    if (rb_scan_args(argc, argv, "01", &rs) == 0) {
	rs = rb_rs;
    }
    rstr_modify(str);
    if (rs == Qnil) {
	return Qnil;
    }
    StringValue(rs);

    const long len = str_length(RSTR(str), false);
    if (len == 0) {
	return Qnil;
    }

    const long rslen = rb_str_chars_len(rs);
    long to_del = 0;

    if (rs == rb_default_rs
	|| rslen == 0
	|| (rslen == 1 && rb_str_get_uchar(rs, 0) == '\n')) {
	UChar c = str_get_uchar(RSTR(str), len - 1, false);
	if (c == '\n') {
	    to_del++;
	    c = str_get_uchar(RSTR(str), len - 2, false);
	}
	if (c == '\r' && (rslen > 0 || to_del != 0)) {
	    to_del++;
	}
    }
    else if (rslen <= len) {
	if (str_index_for_string(RSTR(str), str_need_string(rs),
		    len - rslen, false) >= 0) {
	    to_del += rslen;
	}
    }

    if (to_del == 0) {
	return Qnil;
    }
    str_delete(RSTR(str), len - to_del, to_del, false);
    return str;
}

/*
 *  call-seq:
 *     str.chomp(separator=$/)   => new_str
 *  
 *  Returns a new <code>String</code> with the given record separator removed
 *  from the end of <i>str</i> (if present). If <code>$/</code> has not been
 *  changed from the default Ruby record separator, then <code>chomp</code> also
 *  removes carriage return characters (that is it will remove <code>\n</code>,
 *  <code>\r</code>, and <code>\r\n</code>).
 *     
 *     "hello".chomp            #=> "hello"
 *     "hello\n".chomp          #=> "hello"
 *     "hello\r\n".chomp        #=> "hello"
 *     "hello\n\r".chomp        #=> "hello\n"
 *     "hello\r".chomp          #=> "hello"
 *     "hello \n there".chomp   #=> "hello \n there"
 *     "hello".chomp("llo")     #=> "he"
 */

static VALUE
rstr_chomp(VALUE str, SEL sel, int argc, VALUE *argv)
{
    str = rb_str_new3(str);
    rstr_chomp_bang(str, 0, argc, argv);
    return str;
}

/*
 *  call-seq:
 *     str.sub!(pattern, replacement)          => str or nil
 *     str.sub!(pattern) {|match| block }      => str or nil
 *  
 *  Performs the substitutions of <code>String#sub</code> in place,
 *  returning <i>str</i>, or <code>nil</code> if no substitutions were
 *  performed.
 */

static VALUE
rb_reg_regsub(VALUE str, VALUE src, VALUE match, VALUE regexp)
{
    // TODO
    return str;
}

static VALUE
rstr_sub_bang(VALUE str, SEL sel, int argc, VALUE *argv)
{
    VALUE repl, hash = Qnil;
    bool block_given = false;
    bool tainted = false;

    if (argc == 1 && rb_block_given_p()) {
	block_given = true;
    }
    else if (argc == 2) {
	repl = argv[1];
	hash = rb_check_convert_type(argv[1], T_HASH, "Hash", "to_hash");
	if (NIL_P(hash)) {
	    StringValue(repl);
	}
	if (OBJ_TAINTED(repl)) {
	    tainted = true;
	}
    }
    else {
	rb_raise(rb_eArgError, "wrong number of arguments (%d for 2)", argc);
    }

    VALUE pat = get_pat(argv[0], true);
    if (rb_reg_search(pat, str, 0, false) >= 0) {
	VALUE match = rb_backref_get();

	int count = 0;
	rb_match_result_t *results = rb_reg_match_results(match, &count);
	assert(count > 0);

	if (block_given || !NIL_P(hash)) {
            if (block_given) {
                repl = rb_obj_as_string(rb_yield(rb_reg_nth_match(0, match)));
            }
            else {
                repl = rb_hash_aref(hash, str_substr(str, results[0].beg,
			    results[0].end - results[0].beg));
                repl = rb_obj_as_string(repl);
            }
	    rstr_frozen_check(str);
	    if (block_given) {
		rb_backref_set(match);
	    }
	}
	else {
	    repl = rb_reg_regsub(repl, str, match, pat);
	}

	rstr_modify(str);
	str_splice(RSTR(str), results[0].beg, results[0].end - results[0].beg,
		str_need_string(repl), false);
	if (OBJ_TAINTED(repl)) {
	    tainted = true;
	}

	if (tainted) {
	    OBJ_TAINT(str);
	}
	return str;
    }
    return Qnil;
}

/*
 *  call-seq:
 *     str.sub(pattern, replacement)         => new_str
 *     str.sub(pattern) {|match| block }     => new_str
 *  
 *  Returns a copy of <i>str</i> with the <em>first</em> occurrence of
 *  <i>pattern</i> replaced with either <i>replacement</i> or the value of the
 *  block. The <i>pattern</i> will typically be a <code>Regexp</code>; if it is
 *  a <code>String</code> then no regular expression metacharacters will be
 *  interpreted (that is <code>/\d/</code> will match a digit, but
 *  <code>'\d'</code> will match a backslash followed by a 'd').
 *     
 *  If the method call specifies <i>replacement</i>, special variables such as
 *  <code>$&</code> will not be useful, as substitution into the string occurs
 *  before the pattern match starts. However, the sequences <code>\1</code>,
 *  <code>\2</code>, <code>\k<group_name></code>, etc., may be used.
 *     
 *  In the block form, the current match string is passed in as a parameter, and
 *  variables such as <code>$1</code>, <code>$2</code>, <code>$`</code>,
 *  <code>$&</code>, and <code>$'</code> will be set appropriately. The value
 *  returned by the block will be substituted for the match on each call.
 *     
 *  The result inherits any tainting in the original string or any supplied
 *  replacement string.
 *     
 *     "hello".sub(/[aeiou]/, '*')                  #=> "h*llo"
 *     "hello".sub(/([aeiou])/, '<\1>')             #=> "h<e>llo"
 *     "hello".sub(/./) {|s| s[0].ord.to_s + ' ' }  #=> "104 ello"
 *     "hello".sub(/(?<foo>[aeiou])/, '*\k<foo>*')  #=> "h*e*llo"
 */

static VALUE
rstr_sub(VALUE str, SEL sel, int argc, VALUE *argv)
{
    str = rb_str_new3(str);
    rstr_sub_bang(str, 0, argc, argv);
    return str;
}

// NSString primitives.

static CFIndex
rstr_imp_length(void *rcv, SEL sel)
{
    return str_length(RSTR(rcv), true);
}

static UniChar
rstr_imp_characterAtIndex(void *rcv, SEL sel, CFIndex idx)
{
    return str_get_uchar(RSTR(rcv), idx, true);
}

void
Init_String(void)
{
    // TODO create NSString.m
    rb_cNSString = (VALUE)objc_getClass("NSString");
    assert(rb_cNSString != 0);
    rb_cString = rb_cNSString;
    rb_include_module(rb_cString, rb_mComparable);
    rb_cNSMutableString = (VALUE)objc_getClass("NSMutableString");
    assert(rb_cNSMutableString != 0);

    // rb_cRubyString is defined earlier in Init_PreVM().
    rb_set_class_path(rb_cRubyString, rb_cObject, "String");
    rb_const_set(rb_cObject, rb_intern("String"), rb_cRubyString);

    rb_objc_define_method(*(VALUE *)rb_cRubyString, "alloc", rstr_alloc, 0);
    rb_objc_define_method(rb_cRubyString, "initialize", rstr_initialize, -1);
    rb_objc_define_method(rb_cRubyString, "initialize_copy", rstr_replace, 1);
    rb_objc_define_method(rb_cRubyString, "dup", rstr_dup, 0);
    rb_objc_define_method(rb_cRubyString, "clone", rstr_clone, 0);
    rb_objc_define_method(rb_cRubyString, "replace", rstr_replace, 1);
    rb_objc_define_method(rb_cRubyString, "clear", rstr_clear, 0);
    rb_objc_define_method(rb_cRubyString, "encoding", rstr_encoding, 0);
    rb_objc_define_method(rb_cRubyString, "length", rstr_length, 0);
    rb_objc_define_method(rb_cRubyString, "size", rstr_length, 0); // alias
    rb_objc_define_method(rb_cRubyString, "bytesize", rstr_bytesize, 0);
    rb_objc_define_method(rb_cRubyString, "getbyte", rstr_getbyte, 1);
    rb_objc_define_method(rb_cRubyString, "setbyte", rstr_setbyte, 2);
    rb_objc_define_method(rb_cRubyString, "force_encoding",
	    rstr_force_encoding, 1);
    rb_objc_define_method(rb_cRubyString, "valid_encoding?",
	    rstr_is_valid_encoding, 0);
    rb_objc_define_method(rb_cRubyString, "ascii_only?", rstr_is_ascii_only, 0);
    rb_objc_define_method(rb_cRubyString, "[]", rstr_aref, -1);
    rb_objc_define_method(rb_cRubyString, "slice", rstr_aref, -1);
    rb_objc_define_method(rb_cRubyString, "index", rstr_index, -1);
    rb_objc_define_method(rb_cRubyString, "+", rstr_plus, 1);
    rb_objc_define_method(rb_cRubyString, "<<", rstr_concat, 1);
    rb_objc_define_method(rb_cRubyString, "concat", rstr_concat, 1);
    rb_objc_define_method(rb_cRubyString, "==", rstr_equal, 1);
    rb_objc_define_method(rb_cRubyString, "include?", rstr_includes, 1);
    rb_objc_define_method(rb_cRubyString, "to_s", rstr_to_s, 0);
    rb_objc_define_method(rb_cRubyString, "to_str", rstr_to_s, 0);
    rb_objc_define_method(rb_cRubyString, "to_sym", rstr_intern, 0);
    rb_objc_define_method(rb_cRubyString, "intern", rstr_intern, 0);
    rb_objc_define_method(rb_cRubyString, "inspect", rstr_inspect, 0);
    rb_objc_define_method(rb_cRubyString, "dump", rstr_dump, 0);
    rb_objc_define_method(rb_cRubyString, "match", rstr_match2, -1);
    rb_objc_define_method(rb_cRubyString, "=~", rstr_match, 1);
    rb_objc_define_method(rb_cRubyString, "scan", rstr_scan, 1);
    rb_objc_define_method(rb_cRubyString, "split", rstr_split, -1);
    rb_objc_define_method(rb_cRubyString, "to_i", rstr_to_i, -1);
    rb_objc_define_method(rb_cRubyString, "hex", rstr_hex, 0);
    rb_objc_define_method(rb_cRubyString, "oct", rstr_oct, 0);
    rb_objc_define_method(rb_cRubyString, "chomp", rstr_chomp, -1);
    rb_objc_define_method(rb_cRubyString, "chomp!", rstr_chomp_bang, -1);
    rb_objc_define_method(rb_cRubyString, "sub", rstr_sub, -1);
    rb_objc_define_method(rb_cRubyString, "sub!", rstr_sub_bang, -1);

    // Added for MacRuby (debugging).
    rb_objc_define_method(rb_cRubyString, "__chars_count__",
	    rstr_chars_count, 0);
    rb_objc_define_method(rb_cRubyString, "__getchar__", rstr_getchar, 1);
    rb_objc_define_method(rb_cRubyString, "__stored_in_uchars?__",
	    rstr_is_stored_in_uchars, 0);

    // Cocoa primitives.
    rb_objc_install_method2((Class)rb_cRubyString, "length",
	    (IMP)rstr_imp_length);
    rb_objc_install_method2((Class)rb_cRubyString, "characterAtIndex:",
	    (IMP)rstr_imp_characterAtIndex);
#if 0
    rb_objc_install_method2(rb_cRubyString, "getCharacters:range:",
	    (IMP)rstr_imp_getCharactersRange);
    rb_objc_install_method2(rb_cRubyString,
	    "replaceCharactersInRange:withString:", 
	    (IMP)rstr_imp_replaceCharactersInRangeWithString);
#endif

    rb_fs = Qnil;
    rb_define_variable("$;", &rb_fs);
    rb_define_variable("$-F", &rb_fs);

    // rb_cSymbol is defined earlier in Init_PreVM().
    rb_set_class_path(rb_cSymbol, rb_cObject, "Symbol");
    rb_const_set(rb_cObject, rb_intern("Symbol"), rb_cSymbol);
}

bool
rb_objc_str_is_pure(VALUE str)
{
    VALUE k = *(VALUE *)str;
    while (RCLASS_SINGLETON(k)) {
        k = RCLASS_SUPER(k);
    }
    if (k == rb_cRubyString) {
        return true;
    }
    while (k != 0) {
        if (k == rb_cRubyString) {
            return false;
        }
        k = RCLASS_SUPER(k);
    }
    return true;
}

void
rb_objc_install_string_primitives(Class klass)
{
    // TODO
}

// ByteString emulation.

VALUE
rb_str_bstr(VALUE str)
{
    if (IS_RSTR(str)) {
	str_make_data_binary(RSTR(str));
	return str;
    }
    abort(); // TODO
}

uint8_t *
bstr_bytes(VALUE str)
{
    assert(IS_RSTR(str));
    return (uint8_t *)RSTR(str)->data.bytes;
}

VALUE
bstr_new_with_data(const uint8_t *bytes, long len)
{
    rb_str_t *str = str_alloc(rb_cRubyString);
    str_replace_with_bytes(str, (char *)bytes, len,
	    rb_encodings[ENCODING_BINARY]);
    return (VALUE)str;
}

VALUE
bstr_new(void)
{
    return bstr_new_with_data(NULL, 0);
}

long
bstr_length(VALUE str)
{
    assert(IS_RSTR(str));
    return RSTR(str)->length_in_bytes;
}

void
bstr_resize(VALUE str, long capa)
{
    assert(IS_RSTR(str));
    str_resize_bytes(RSTR(str), capa);
}

void
bstr_set_length(VALUE str, long len)
{
    assert(IS_RSTR(str));
    assert(len < RSTR(str)->capacity_in_bytes);
    RSTR(str)->length_in_bytes = len;
}

// Compiler primitives.

VALUE
rb_str_new_empty(void)
{
    return (VALUE)str_alloc(rb_cRubyString);
}

VALUE
rb_unicode_str_new(const UniChar *ptr, const size_t len)
{
    VALUE str = rb_str_new_empty();
    str_replace_with_uchars(RSTR(str), ptr, len);
    return str;
}

VALUE
rb_str_new_fast(int argc, ...)
{
    VALUE str = (VALUE)str_alloc(rb_cRubyString);

    if (argc > 0) {
	va_list ar;
	va_start(ar, argc);
	for (int i = 0; i < argc; ++i) {
	    VALUE fragment = va_arg(ar, VALUE);
	    switch (TYPE(fragment)) {
		default:
		    fragment = rb_obj_as_string(fragment);
		    // fall through

		case T_STRING:
		    rstr_concat(str, 0, fragment);
		    break;
	    }
	}
	va_end(ar);
    }

    return str;
}

// MRI C-API compatibility.

VALUE
rb_enc_str_new(const char *cstr, long len, rb_encoding_t *enc)
{
    // XXX should we assert that enc is single byte?
    if (enc == NULL) {
	// This function can be called with a NULL encoding. 
	enc = rb_encodings[ENCODING_UTF8];
    }
    rb_str_t *str = str_alloc(rb_cRubyString);
    str_replace_with_bytes(str, cstr, len, enc);
    return (VALUE)str;
}

VALUE
rb_str_new(const char *cstr, long len)
{
    return rb_enc_str_new(cstr, len, rb_encodings[ENCODING_UTF8]);
}

VALUE
rb_str_buf_new(long len)
{
    return rb_str_new(NULL, len);
}

VALUE
rb_str_new2(const char *cstr)
{
    return rb_str_new(cstr, strlen(cstr));
}

VALUE
rb_str_new3(VALUE source)
{
    rb_str_t *str = str_alloc(rb_cRubyString);
    str_replace(str, source);
    return (VALUE)str;
}

VALUE
rb_str_new4(VALUE source)
{
    VALUE str = rb_str_new3(source);
    OBJ_FREEZE(str);
    return str;
}

VALUE
rb_tainted_str_new(const char *cstr, long len)
{
    VALUE str = rb_str_new(cstr, len);
    OBJ_TAINT(str);
    return str;
}

VALUE
rb_tainted_str_new2(const char *cstr)
{
    return rb_tainted_str_new(cstr, strlen(cstr));
}

VALUE
rb_usascii_str_new(const char *cstr, long len)
{
    VALUE str = rb_str_new(cstr, len);
    RSTR(str)->encoding = rb_encodings[ENCODING_ASCII];
    return str;
}

VALUE
rb_usascii_str_new2(const char *cstr)
{
    return rb_usascii_str_new(cstr, strlen(cstr));
}

const char *
rb_str_cstr(VALUE str)
{
    if (IS_RSTR(str)) {
	str_make_data_binary(RSTR(str));
	return RSTR(str)->data.bytes;
    }

    // CFString code path, hopefully this should not happen a lot.
    const char *cptr = (const char *)CFStringGetCStringPtr((CFStringRef)str, 0);
    if (cptr != NULL) {
	return cptr;
    }

    const long max = CFStringGetMaximumSizeForEncoding(
	    CFStringGetLength((CFStringRef)str),
	    kCFStringEncodingUTF8);
    char *cptr2 = (char *)xmalloc(max + 1);
    if (!CFStringGetCString((CFStringRef)str, cptr2, max + 1,
		kCFStringEncodingUTF8)) {
	// Probably an UTF16 string...
	xfree(cptr2);
	return NULL;
    }
    return cptr2;
}

long
rb_str_clen(VALUE str)
{
    if (IS_RSTR(str)) {
	str_make_data_binary(RSTR(str));
	return RSTR(str)->length_in_bytes;
    }
    return CFStringGetLength((CFStringRef)str);
}

char *
rb_string_value_cstr(volatile VALUE *ptr)
{
    VALUE str = rb_string_value(ptr);
    return (char *)rb_str_cstr(str);
}

char *
rb_string_value_ptr(volatile VALUE *ptr)
{
    return rb_string_value_cstr(ptr);
}

VALUE
rb_string_value(volatile VALUE *ptr)
{
    VALUE s = *ptr;
    if (TYPE(s) != T_STRING) {
	s = rb_str_to_str(s);
	*ptr = s;
    }
    return s;
}

VALUE
rb_check_string_type(VALUE str)
{
    return rb_check_convert_type(str, T_STRING, "String", "to_str");
}

VALUE
rb_str_to_str(VALUE str)
{
    return rb_convert_type(str, T_STRING, "String", "to_str");
}

VALUE
rb_obj_as_string(VALUE obj)
{
    if (TYPE(obj) == T_STRING || TYPE(obj) == T_SYMBOL) {
	return obj;
    }
    VALUE str = rb_vm_call(obj, selToS, 0, NULL, false);
    if (TYPE(str) != T_STRING) {
	return rb_any_to_s(obj);
    }
    if (OBJ_TAINTED(obj)) {
	OBJ_TAINT(str);
    }
    return str;
}

void
rb_str_setter(VALUE val, ID id, VALUE *var)
{
    if (!NIL_P(val) && TYPE(val) != T_STRING) {
	rb_raise(rb_eTypeError, "value of %s must be String", rb_id2name(id));
    }
    *var = val;
}

ID
rb_to_id(VALUE name)
{
    VALUE tmp;
    switch (TYPE(name)) {
	default:
	    tmp = rb_check_string_type(name);
	    if (NIL_P(tmp)) {
		rb_raise(rb_eTypeError, "%s is not a symbol",
			RSTRING_PTR(rb_inspect(name)));
	    }
	    name = tmp;
	    /* fall through */
	case T_STRING:
	    name = rstr_intern(name, 0);
	    /* fall through */
	case T_SYMBOL:
	    return SYM2ID(name);
    }
}

UChar
rb_str_get_uchar(VALUE str, long pos)
{
    if (RSTR(str)) {
	return str_get_uchar(RSTR(str), pos, false);
    }
    assert(pos >= 0 && pos < CFStringGetLength((CFStringRef)str));
    return CFStringGetCharacterAtIndex((CFStringRef)str, pos);
}

long
rb_str_chars_len(VALUE str)
{
    if (IS_RSTR(str)) {
	return str_length(RSTR(str), false);
    }
    return CFStringGetLength((CFStringRef)str);
}

VALUE
rb_str_length(VALUE str)
{
    return LONG2NUM(rb_str_chars_len(str));
}

VALUE
rb_str_buf_new2(const char *cstr)
{
    return rb_str_new2(cstr);
}

VALUE
rb_enc_str_buf_cat(VALUE str, const char *cstr, long len, rb_encoding_t *enc)
{
    if (IS_RSTR(str)) {
	// XXX this could be optimized
	VALUE substr = rb_enc_str_new(cstr, len, enc);
	str_concat_string(RSTR(str), RSTR(substr));
    }
    else {
	abort(); // TODO	
    }
    return str;
}

VALUE
rb_str_buf_cat(VALUE str, const char *cstr, long len)
{
    return rb_enc_str_buf_cat(str, cstr, len, RSTR(str)->encoding);
}

VALUE
rb_str_buf_cat2(VALUE str, const char *cstr)
{
    return rb_str_buf_cat(str, cstr, strlen(cstr));
}

VALUE
rb_str_cat(VALUE str, const char *cstr, long len)
{
    return rb_str_buf_cat(str, cstr, len);
}

VALUE
rb_str_cat2(VALUE str, const char *cstr)
{
    return rb_str_buf_cat2(str, cstr);
}

VALUE
rb_str_buf_cat_ascii(VALUE str, const char *cstr)
{
    return rb_str_buf_cat2(str, cstr);
}

VALUE
rb_str_buf_append(VALUE str, VALUE str2)
{
    if (IS_RSTR(str)) {
	return rstr_concat(str, 0, str2);
    }
    CFStringAppend((CFMutableStringRef)str, (CFStringRef)str2);
    return str;
}

VALUE
rb_str_append(VALUE str, VALUE str2)
{
    return rb_str_buf_append(str, str2);
}

VALUE
rb_str_concat(VALUE str, VALUE str2)
{
    return rb_str_buf_append(str, str2);
}

void
rb_str_associate(VALUE str, VALUE add)
{
    // Do nothing.
}

VALUE
rb_str_associated(VALUE str)
{
    // Do nothing.
    return Qfalse;
}

VALUE
rb_str_resize(VALUE str, long len)
{
    if (IS_RSTR(str)) {
	str_resize_bytes(RSTR(str), len);
    }
    else {
	abort(); // TODO
    }
    return str;
}

VALUE
rb_str_equal(VALUE str, VALUE str2)
{
    if (IS_RSTR(str)) {
	return rstr_equal(str, 0, str2);
    }
    return CFEqual((CFStringRef)str, (CFStringRef)str2) ? Qtrue : Qfalse;
}

VALUE
rb_str_dup(VALUE str)
{
    if (IS_RSTR(str)) {
	return (VALUE)str_dup(RSTR(str));
    }
    if (TYPE(str) == T_SYMBOL) {
	return rb_str_new2(RSYMBOL(str)->str);
    }
    abort(); // TODO
}

int
rb_memhash(const void *ptr, long len)
{
    CFDataRef data = CFDataCreate(NULL, (const UInt8 *)ptr, len);
    int code = CFHash(data);
    CFRelease((CFTypeRef)data);
    return code;
}

VALUE
rb_str_inspect(VALUE rcv)
{
    if (RSTR(rcv)) {
	return rstr_inspect(rcv, 0);
    }
    // TODO
    return rcv;
}

VALUE
rb_str_subseq(VALUE str, long beg, long len)
{
    if (IS_RSTR(str)) {
	return str_substr(str, beg, len);
    }
    abort(); // TODO
}

VALUE
rb_str_substr(VALUE str, long beg, long len)
{
    return rb_str_subseq(str, beg, len);
}

void
rb_str_update(VALUE str, long beg, long len, VALUE val)
{
    abort(); // TODO
}

// Symbols (TODO: move me outside)

VALUE
rb_sym_to_s(VALUE sym)
{
    return rb_str_new2(RSYMBOL(sym)->str);
}
