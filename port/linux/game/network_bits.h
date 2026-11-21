/*
NETWORK_BITS.H

The host's packed states' bits (network_distributed.c's tick states as
differences, network_objects.c's objects' states): values written in as
few bits as they need, least significant bit first, and read back no
further than the bytes they came in. Every value reads back exactly as
written; where one does not fit, the writer is told (failed) and writes
the whole form in its place.
*/

#ifndef NETWORK_BITS_H
#define NETWORK_BITS_H

/* ---------- constants */

enum
{
	/* a signed value's width goes in 5 bits: 0 (the value 0) to 31 */
	NETWORK_BITS_WIDTH_BITS = 5,
	NETWORK_BITS_MAXIMUM_WIDTH = 31,
	/* a unit vector's (network_bits_put_unit_vector): its largest part's
	index in 2 bits (NETWORK_BITS_UNIT_WHOLE: none, all three parts follow),
	and how far that part is from the length the other two leave it, from
	NETWORK_BITS_UNIT_LEAST, in 2 bits */
	NETWORK_BITS_UNIT_WHOLE = 3,
	NETWORK_BITS_UNIT_LEAST = -1,
	NETWORK_BITS_UNIT_REST_BITS = 2,
	/* (a unit vector's length in its parts: DISTRIBUTED_UNIT_SCALE) */
	NETWORK_BITS_UNIT_LENGTH = 32767,
};

/* ---------- structures */

/* bits written to (data) or read from (read), size bytes of them at most;
failed once a value went past them, or did not fit its bits */
struct network_bits
{
	byte *data;
	byte const *read;
	long size;
	long position;
	boolean failed;
};

/* ---------- code */

static inline void network_bits_begin_write(
	struct network_bits *bits,
	byte *data,
	long size)
{
	bits->data = data;
	bits->read = data;
	bits->size = size;
	bits->position = 0;
	bits->failed = FALSE;
}

static inline void network_bits_begin_read(
	struct network_bits *bits,
	byte const *data,
	long size)
{
	bits->data = NULL;
	bits->read = data;
	bits->size = size;
	bits->position = 0;
	bits->failed = FALSE;
}

/* the whole bytes the bits written or read so far take */
static inline long network_bits_bytes(
	struct network_bits const *bits)
{
	return (bits->position + 7) >> 3;
}

/* the low count bits of value (count at most 32) */
static inline void network_bits_put(
	struct network_bits *bits,
	unsigned long value,
	short count)
{
	short index;

	for (index = 0; index < count; index++, bits->position++)
	{
		long byte_index = bits->position >> 3;

		if (byte_index >= bits->size)
		{
			bits->failed = TRUE;
			return;
		}
		if (!(bits->position & 7))
			bits->data[byte_index] = 0;
		if ((value >> index) & 1)
			bits->data[byte_index] |= (byte)(1 << (bits->position & 7));
	}
}

static inline unsigned long network_bits_take(
	struct network_bits *bits,
	short count)
{
	unsigned long value = 0;
	short index;

	for (index = 0; index < count; index++, bits->position++)
	{
		long byte_index = bits->position >> 3;

		if (byte_index >= bits->size)
		{
			bits->failed = TRUE;
			return 0;
		}
		if ((bits->read[byte_index] >> (bits->position & 7)) & 1)
			value |= 1UL << index;
	}
	return value;
}

/* the bits a signed value takes in two's complement: 0 for 0 */
static inline short network_bits_width(
	long value)
{
	short width = 1;

	if (!value)
		return 0;
	if (value < 0)
		value = ~value;
	while (value)
	{
		width++;
		value >>= 1;
	}
	return width;
}

/* values in one width, written first (5 bits) */
static inline void network_bits_put_group(
	struct network_bits *bits,
	long const *values,
	short count)
{
	short width = 0;
	short index;

	for (index = 0; index < count; index++)
	{
		short value_width = network_bits_width(values[index]);

		if (value_width > width)
			width = value_width;
	}
	if (width > NETWORK_BITS_MAXIMUM_WIDTH)
	{
		bits->failed = TRUE;
		return;
	}
	network_bits_put(bits, (unsigned long)width, NETWORK_BITS_WIDTH_BITS);
	for (index = 0; index < count; index++)
		network_bits_put(bits, (unsigned long)values[index] & ((1UL << width) - 1), width);
}

static inline void network_bits_take_group(
	struct network_bits *bits,
	long *values,
	short count)
{
	short width = (short)network_bits_take(bits, NETWORK_BITS_WIDTH_BITS);
	short index;

	for (index = 0; index < count; index++)
	{
		unsigned long value = network_bits_take(bits, width);

		/* (its sign, extended) */
		if (width && ((value >> (width - 1)) & 1))
			value |= ~0UL << width;
		values[index] = (long)value;
	}
}

/* the whole number no greater than value's square root */
static inline unsigned long network_bits_square_root(
	unsigned long value)
{
	unsigned long root = 0;
	unsigned long bit = 1UL << 30;

	while (bit > value)
		bit >>= 2;
	while (bit)
	{
		if (value >= root + bit)
		{
			value -= root + bit;
			root = (root >> 1) + bit;
		}
		else
			root >>= 1;
		bit >>= 2;
	}
	return root;
}

/* (a unit vector of NETWORK_BITS_UNIT_LENGTH) the length its largest part
has where the other two are a and b: NONE where they leave it none */
static inline long network_bits_unit_rest(
	long a,
	long b)
{
	long left = NETWORK_BITS_UNIT_LENGTH * NETWORK_BITS_UNIT_LENGTH - a * a - b * b;

	return left < 0 ? NONE : (long)network_bits_square_root((unsigned long)left);
}

/* a vector of three 16-bit parts that is about a unit one (of
NETWORK_BITS_UNIT_LENGTH), each part its difference from base's (NULL: from
0), exactly: the largest part's index, its sign and how far it is from the
length the other two leave it, then the other two (a width each, separate,
or one width for both), or (NETWORK_BITS_UNIT_WHOLE) all three, where the
largest is further from that length than its 2 bits tell */
static inline void network_bits_put_unit_vector(
	struct network_bits *bits,
	short const *parts,
	short const *base,
	boolean separate)
{
	long others[3];
	short largest = 0;
	short axis;
	short count = 0;
	long rest;
	long length;

	for (axis = 1; axis < 3; axis++)
	{
		if ((parts[axis] < 0 ? -(long)parts[axis] : (long)parts[axis]) >
			(parts[largest] < 0 ? -(long)parts[largest] : (long)parts[largest]))
		{
			largest = axis;
		}
	}
	for (axis = 0; axis < 3; axis++)
	{
		if (axis != largest)
			others[count++] = parts[axis];
	}
	length = network_bits_unit_rest(others[0], others[1]);
	rest = (parts[largest] < 0 ? -(long)parts[largest] : (long)parts[largest]) - length;
	if (length == NONE || rest < NETWORK_BITS_UNIT_LEAST ||
		rest >= NETWORK_BITS_UNIT_LEAST + (1 << NETWORK_BITS_UNIT_REST_BITS))
	{
		network_bits_put(bits, NETWORK_BITS_UNIT_WHOLE, 2);
		largest = NONE;
	}
	else
	{
		network_bits_put(bits, (unsigned long)largest, 2);
		network_bits_put(bits, parts[largest] < 0 ? 1 : 0, 1);
		network_bits_put(bits, (unsigned long)(rest - NETWORK_BITS_UNIT_LEAST), NETWORK_BITS_UNIT_REST_BITS);
	}
	count = 0;
	for (axis = 0; axis < 3; axis++)
	{
		if (axis != largest)
			others[count++] = (long)parts[axis] - (base ? (long)base[axis] : 0);
	}
	if (separate)
	{
		for (axis = 0; axis < count; axis++)
			network_bits_put_group(bits, &others[axis], 1);
	}
	else
		network_bits_put_group(bits, others, count);
}

/* ... read: FALSE for parts that make no 16-bit vector */
static inline boolean network_bits_take_unit_vector(
	struct network_bits *bits,
	short *parts,
	short const *base,
	boolean separate)
{
	long others[3];
	long values[3];
	short largest = (short)network_bits_take(bits, 2);
	short negative = 0;
	long rest = 0;
	short count = largest == NETWORK_BITS_UNIT_WHOLE ? 3 : 2;
	short axis;
	short other = 0;

	if (largest != NETWORK_BITS_UNIT_WHOLE)
	{
		negative = (short)network_bits_take(bits, 1);
		rest = (long)network_bits_take(bits, NETWORK_BITS_UNIT_REST_BITS) + NETWORK_BITS_UNIT_LEAST;
	}
	if (separate)
	{
		for (axis = 0; axis < count; axis++)
			network_bits_take_group(bits, &others[axis], 1);
	}
	else
		network_bits_take_group(bits, others, count);
	if (bits->failed)
		return FALSE;
	for (axis = 0; axis < 3; axis++)
	{
		if (axis == largest)
			continue;
		values[axis] = others[other++] + (base ? (long)base[axis] : 0);
		if (values[axis] < -32768 || values[axis] > 32767)
			return FALSE;
	}
	if (largest != NETWORK_BITS_UNIT_WHOLE)
	{
		long length = network_bits_unit_rest(values[largest == 0 ? 1 : 0], values[largest == 2 ? 1 : 2]);

		if (length == NONE)
			return FALSE;
		values[largest] = (negative ? -1 : 1) * (length + rest);
		if (values[largest] < -32768 || values[largest] > 32767)
			return FALSE;
	}
	for (axis = 0; axis < 3; axis++)
		parts[axis] = (short)values[axis];
	return TRUE;
}

#endif
