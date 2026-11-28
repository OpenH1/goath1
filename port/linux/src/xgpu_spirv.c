/*
XGPU_SPIRV.C

SPIR-V from the shader intermediate form (xgpu_shader.h), for the Vulkan
backend; the conventions are in xgpu_spirv.h. The programs are OpenCE's
GLSL emitter's operation for operation, each spelled as glslang spells the
GLSL (its comparisons, its clamps, its smeared scalars), and the Metal
emitter's (xgpu_msl.c) are this file's, so that the two backends draw
alike.

The words are written directly. A program has no control flow but its
discards (and the enhanced dynamic lights' loops), so every register is an
SSA value: a write to part of a register is a new value made of the old one
and the result (OpVectorShuffle), and the program's values are never
stored. The module's types and constants are made once each (interned).
*/

#include "xgpu.h"
#include "xgpu_backend.h"
#include "xgpu_hdr.h"
#include "xgpu_shader.h"
#include "xgpu_spirv.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- SPIR-V's numbers (the specification's, version 1.3) */

enum
{
	SPV_MAGIC = 0x07230203,
	SPV_VERSION = 0x00010300,
};

enum
{
	OP_NAME = 5, OP_EXTENSION = 10, OP_EXT_INST_IMPORT = 11, OP_EXT_INST = 12, OP_MEMORY_MODEL = 14,
	OP_ENTRY_POINT = 15, OP_EXECUTION_MODE = 16, OP_CAPABILITY = 17,
	OP_TYPE_VOID = 19, OP_TYPE_BOOL = 20, OP_TYPE_INT = 21, OP_TYPE_FLOAT = 22, OP_TYPE_VECTOR = 23,
	OP_TYPE_IMAGE = 25, OP_TYPE_SAMPLED_IMAGE = 27, OP_TYPE_ARRAY = 28, OP_TYPE_RUNTIME_ARRAY = 29, OP_TYPE_STRUCT = 30, OP_TYPE_POINTER = 32,
	OP_TYPE_FUNCTION = 33,
	OP_CONSTANT_TRUE = 41, OP_CONSTANT_FALSE = 42, OP_CONSTANT = 43, OP_CONSTANT_COMPOSITE = 44,
	OP_FUNCTION = 54, OP_FUNCTION_END = 56, OP_VARIABLE = 59, OP_LOAD = 61, OP_STORE = 62, OP_ACCESS_CHAIN = 65,
	OP_DECORATE = 71, OP_MEMBER_DECORATE = 72,
	OP_VECTOR_EXTRACT_DYNAMIC = 77, OP_VECTOR_SHUFFLE = 79, OP_COMPOSITE_CONSTRUCT = 80, OP_COMPOSITE_EXTRACT = 81,
	OP_COMPOSITE_INSERT = 82,
	OP_IMAGE_SAMPLE_IMPLICIT_LOD = 87, OP_IMAGE_SAMPLE_EXPLICIT_LOD = 88, OP_IMAGE_FETCH = 95, OP_IMAGE_READ = 98, OP_IMAGE_WRITE = 99, OP_IMAGE = 100, OP_IMAGE_QUERY_SIZE_LOD = 103,
	OP_CONVERT_F_TO_S = 110, OP_CONVERT_S_TO_F = 111, OP_BITCAST = 124,
	OP_F_NEGATE = 127, OP_I_ADD = 128, OP_F_ADD = 129, OP_I_SUB = 130, OP_F_SUB = 131, OP_I_MUL = 132, OP_F_MUL = 133,
	OP_F_DIV = 136, OP_VECTOR_TIMES_SCALAR = 142, OP_DOT = 148,
	OP_LOGICAL_OR = 166, OP_LOGICAL_AND = 167, OP_LOGICAL_NOT = 168, OP_SELECT = 169,
	OP_I_EQUAL = 170, OP_I_NOT_EQUAL = 171, OP_S_LESS_THAN = 177,
	OP_F_ORD_EQUAL = 180, OP_F_UNORD_NOT_EQUAL = 183, OP_F_ORD_LESS_THAN = 184, OP_F_ORD_GREATER_THAN = 186,
	OP_F_ORD_LESS_THAN_EQUAL = 188, OP_F_ORD_GREATER_THAN_EQUAL = 190,
	OP_SHIFT_RIGHT_ARITHMETIC = 195, OP_SHIFT_LEFT_LOGICAL = 196, OP_BITWISE_OR = 197, OP_BITWISE_XOR = 198, OP_BITWISE_AND = 199,
	OP_CONVERT_U_TO_F = 112,
	OP_ANY = 154, OP_U_GREATER_THAN_EQUAL = 174, OP_U_LESS_THAN = 176, OP_SHIFT_RIGHT_LOGICAL = 194,
	OP_GROUP_NON_UNIFORM_BITWISE_OR = 359,
	OP_DPDX = 207, OP_DPDY = 208, OP_FWIDTH = 209, OP_ATOMIC_I_ADD = 234,
	OP_LOOP_MERGE = 246, OP_SELECTION_MERGE = 247, OP_LABEL = 248, OP_BRANCH = 249, OP_BRANCH_CONDITIONAL = 250,
	OP_KILL = 252, OP_RETURN = 253,
	/* SPV_KHR_ray_query's (display.ambient_occlusion "ray_traced") */
	OP_TYPE_RAY_QUERY = 4472, OP_RAY_QUERY_INITIALIZE = 4473, OP_RAY_QUERY_CONFIRM_INTERSECTION = 4476,
	OP_RAY_QUERY_PROCEED = 4477, OP_RAY_QUERY_GET_INTERSECTION_TYPE = 4479, OP_TYPE_ACCELERATION_STRUCTURE = 5341,
	OP_RAY_QUERY_GET_INTERSECTION_T = 6018, OP_RAY_QUERY_GET_INTERSECTION_INSTANCE_CUSTOM_INDEX = 6019,
	OP_RAY_QUERY_GET_INTERSECTION_INSTANCE_SHADER_BINDING_TABLE_RECORD_OFFSET = 6021,
	OP_RAY_QUERY_GET_INTERSECTION_GEOMETRY_INDEX = 6022, OP_RAY_QUERY_GET_INTERSECTION_PRIMITIVE_INDEX = 6023,
	OP_RAY_QUERY_GET_INTERSECTION_BARYCENTRICS = 6024,
};

/* GLSL.std.450's instructions */
enum
{
	GLSL_F_ABS = 4, GLSL_F_SIGN = 6, GLSL_ACOS = 17, GLSL_ATAN = 18, GLSL_ATAN2 = 25, GLSL_FLOOR = 8, GLSL_FRACT = 10, GLSL_SIN = 13, GLSL_COS = 14, GLSL_POW = 26, GLSL_EXP = 27, GLSL_EXP2 = 29,
	GLSL_LOG2 = 30, GLSL_SQRT = 31, GLSL_INVERSE_SQRT = 32, GLSL_F_MIN = 37, GLSL_F_MAX = 40, GLSL_F_CLAMP = 43,
	GLSL_U_MIN = 38, GLSL_S_MIN = 39, GLSL_S_MAX = 42, GLSL_S_CLAMP = 45, GLSL_F_MIX = 46, GLSL_SMOOTH_STEP = 49, GLSL_LENGTH = 66, GLSL_CROSS = 68, GLSL_NORMALIZE = 69, GLSL_FIND_I_LSB = 73,
	GLSL_PACK_SNORM_2X16 = 56, GLSL_PACK_HALF_2X16 = 58, GLSL_UNPACK_SNORM_2X16 = 60, GLSL_UNPACK_HALF_2X16 = 62,
};

enum
{
	DECORATION_BLOCK = 2, DECORATION_ARRAY_STRIDE = 6, DECORATION_BUILT_IN = 11, DECORATION_FLAT = 14, DECORATION_INVARIANT = 18,
	DECORATION_COHERENT = 23, DECORATION_NON_WRITABLE = 24, DECORATION_LOCATION = 30, DECORATION_BINDING = 33, DECORATION_DESCRIPTOR_SET = 34,
	DECORATION_OFFSET = 35, DECORATION_NO_CONTRACTION = 42, DECORATION_INPUT_ATTACHMENT_INDEX = 43,
};

enum
{
	BUILT_IN_POSITION = 0, BUILT_IN_POINT_SIZE = 1, BUILT_IN_FRAG_COORD = 15, BUILT_IN_SAMPLE_MASK = 20,
	BUILT_IN_SAMPLE_ID = 18, BUILT_IN_FRAG_DEPTH = 22,
	BUILT_IN_VIEW_INDEX = 4440, BUILT_IN_VERTEX_INDEX = 42,
};

enum
{
	STORAGE_UNIFORM_CONSTANT = 0, STORAGE_INPUT = 1, STORAGE_UNIFORM = 2, STORAGE_OUTPUT = 3, STORAGE_FUNCTION = 7,
	STORAGE_STORAGE_BUFFER = 12,
};

enum
{
	CAPABILITY_SHADER = 1, CAPABILITY_STORAGE_IMAGE_MULTISAMPLE = 27, CAPABILITY_SAMPLE_RATE_SHADING = 35, CAPABILITY_INPUT_ATTACHMENT = 40,
	CAPABILITY_IMAGE_QUERY = 50, CAPABILITY_GROUP_NON_UNIFORM = 61, CAPABILITY_GROUP_NON_UNIFORM_ARITHMETIC = 63,
	CAPABILITY_MULTIVIEW = 4439, CAPABILITY_RAY_QUERY = 4472,
	/* SPV_EXT_descriptor_indexing's (the ray-traced scene's textures' slots,
	sampled per pixel) */
	CAPABILITY_SHADER_NON_UNIFORM = 5301, CAPABILITY_SAMPLED_IMAGE_ARRAY_NON_UNIFORM_INDEXING = 5307,
	DECORATION_NON_UNIFORM = 5300,
	CAPABILITY_FRAGMENT_SHADER_SAMPLE_INTERLOCK = 5363, CAPABILITY_FRAGMENT_SHADER_PIXEL_INTERLOCK = 5378,
	OP_BEGIN_INVOCATION_INTERLOCK = 5364, OP_END_INVOCATION_INTERLOCK = 5365,
	RAY_FLAGS_CULL_BACK_FACING_TRIANGLES = 0x10, RAY_QUERY_CANDIDATE = 0, RAY_QUERY_COMMITTED = 1,
	EXECUTION_MODEL_VERTEX = 0, EXECUTION_MODEL_FRAGMENT = 4, EXECUTION_MODEL_GL_COMPUTE = 5,
	EXECUTION_MODE_LOCAL_SIZE = 17, BUILT_IN_GLOBAL_INVOCATION_ID = 28,
	EXECUTION_MODE_ORIGIN_UPPER_LEFT = 7, EXECUTION_MODE_EARLY_FRAGMENT_TESTS = 9, EXECUTION_MODE_DEPTH_REPLACING = 12,
	EXECUTION_MODE_DEPTH_GREATER = 14, EXECUTION_MODE_DEPTH_LESS = 15,
	EXECUTION_MODE_PIXEL_INTERLOCK_ORDERED = 5366, EXECUTION_MODE_SAMPLE_INTERLOCK_ORDERED = 5368,
	DIM_2D = 1, DIM_3D = 2, DIM_CUBE = 3, DIM_SUBPASS_DATA = 6, IMAGE_FORMAT_RGBA16F = 2,
	IMAGE_OPERANDS_BIAS = 1, IMAGE_OPERANDS_LOD = 2, IMAGE_OPERANDS_GRAD = 4, IMAGE_OPERANDS_SAMPLE = 0x40,
	SCOPE_DEVICE = 1, SCOPE_SUBGROUP = 3, GROUP_OPERATION_REDUCE = 0,
};

/* ---------- the module being written */

struct words
{
	unsigned int *words;
	unsigned long count, capacity;
};

/* a type or constant made once: its instruction's words but its result */
struct interned
{
	unsigned long start, count;
	unsigned int id;
};

/* the sections a module's instructions go in, in the order the
specification lays them out */
enum
{
	SECTION_CAPABILITIES, SECTION_EXTENSIONS, SECTION_IMPORTS, SECTION_MEMORY_MODEL, SECTION_ENTRY_POINTS,
	SECTION_EXECUTION_MODES, SECTION_NAMES, SECTION_DECORATIONS, SECTION_GLOBALS, SECTION_VARIABLES, SECTION_CODE,
	SECTION_COUNT,
};

struct module
{
	struct words sections[SECTION_COUNT];
	/* the interned instructions' words (their keys), and where each is */
	struct words keys;
	struct interned *interned;
	unsigned long interned_count, interned_capacity;
	unsigned int next_id;
	BOOL failed;
	/* the entry point's interface (its Input and Output variables) */
	unsigned int interface[64];
	int interface_count;
	/* textures' sizes are asked (the ImageQuery capability); a subgroup's
bitwise or (GroupNonUniformArithmetic) */
	BOOL image_query, group_non_uniform;
	/* the GPU's ray queries are traced (the RayQueryKHR capability,
	SPV_KHR_ray_query); and their hits' set read (its table and textures'
	variables, 0 until declared: the textures indexed per pixel,
	SPV_EXT_descriptor_indexing) */
	BOOL ray_query;
	unsigned int rt_hit_table, rt_hit_textures;
	/* (display.rt_reflections: the ray-traced scene a surface's reflection
	traces, 0 until declared: rt_reflection_texel) */
	unsigned int rt_reflection_scene;

	unsigned int glsl;
	unsigned int t_void, t_bool, t_int, t_uint, t_float;
	unsigned int t_v2, t_v3, t_v4, t_iv2, t_iv3, t_iv4, t_uv4, t_bv2, t_bv3, t_bv4;
	/* the uniform blocks' variables (0 until declared) */
	unsigned int constants, uniforms, dynamic_lights, dynamic_light_cells, samples, view_index;
	/* (the enhanced flashlight's shadow map: 0 until declared, flashlight_shadow_declare) */
	unsigned int flashlight_shadow;
	/* (display.ambient_occlusion "world": the window's block and the
	structure's distance field) */
	unsigned int world_ao, world_sdf, world_sdf_table, world_ao_scenery, world_ao_objects;
	/* (found once a pixel: its texture, and the passes' inputs) */
	unsigned int world_ao_pixels, t_uv2;
	/* the vertex constants read (xgpu_spirv_module's) */
	unsigned long long constants_read[3];
	/* display.temporal_aa's motion (xgpu_spirv_options' motion): the constants'
	block holds the previous frame's registers after the 192, and the
	program reads them while previous is set */
	BOOL motion, previous;
};

static void words_push(struct module *m, struct words *words, unsigned int word)
{
	if (words->count == words->capacity)
	{
		unsigned long capacity = words->capacity ? words->capacity * 2 : 256;
		unsigned int *grown = realloc(words->words, capacity * sizeof(*grown));

		if (!grown)
		{
			m->failed = TRUE;
			return;
		}
		words->words = grown;
		words->capacity = capacity;
	}
	words->words[words->count++] = word;
}

static unsigned int id_new(struct module *m)
{
	return m->next_id++;
}

/* an instruction of count operand words into a section */
static void instruction(struct module *m, int section, unsigned int opcode, int count, const unsigned int *operands)
{
	struct words *words = &m->sections[section];
	int index;

	words_push(m, words, opcode | ((unsigned int)(count + 1) << 16));
	for (index = 0; index < count; index++)
		words_push(m, words, operands[index]);
}

/* a string's words (nul-terminated, padded to a word), after operands */
static void instruction_string(struct module *m, int section, unsigned int opcode, int count, const unsigned int *operands,
	const char *string, int trailing_count, const unsigned int *trailing)
{
	struct words *words = &m->sections[section];
	unsigned long length = strlen(string), string_words = length / 4 + 1, index;

	words_push(m, words, opcode | ((unsigned int)(1 + count + string_words + trailing_count) << 16));
	for (index = 0; index < (unsigned long)count; index++)
		words_push(m, words, operands[index]);
	for (index = 0; index < string_words; index++)
	{
		unsigned int word = 0;
		int byte;

		for (byte = 0; byte < 4; byte++)
		{
			unsigned long at = index * 4 + byte;

			if (at < length)
				word |= (unsigned int)(unsigned char)string[at] << (byte * 8);
		}
		words_push(m, words, word);
	}
	for (index = 0; index < (unsigned long)trailing_count; index++)
		words_push(m, words, trailing[index]);
}

/* a type (result first) or a constant (its type, then its result) made
once: the operands are the instruction's but its result */
static unsigned int intern(struct module *m, unsigned int opcode, BOOL typed, int count, const unsigned int *operands)
{
	unsigned int words[24];
	unsigned long index;
	unsigned int id;

	if (count > 22)
	{
		m->failed = TRUE;
		return 0;
	}
	words[0] = opcode;
	memcpy(words + 1, operands, count * sizeof(*operands));
	for (index = 0; index < m->interned_count; index++)
	{
		const struct interned *entry = &m->interned[index];

		if (entry->count == (unsigned long)count + 1 && !memcmp(m->keys.words + entry->start, words, (count + 1) * sizeof(*words)))
			return entry->id;
	}
	if (m->interned_count == m->interned_capacity)
	{
		unsigned long capacity = m->interned_capacity ? m->interned_capacity * 2 : 128;
		struct interned *grown = realloc(m->interned, capacity * sizeof(*grown));

		if (!grown)
		{
			m->failed = TRUE;
			return 0;
		}
		m->interned = grown;
		m->interned_capacity = capacity;
	}
	id = id_new(m);
	m->interned[m->interned_count].start = m->keys.count;
	m->interned[m->interned_count].count = count + 1;
	m->interned[m->interned_count].id = id;
	m->interned_count++;
	for (index = 0; index <= (unsigned long)count; index++)
		words_push(m, &m->keys, words[index]);
	{
		unsigned int out[24];

		if (typed)
		{
			out[0] = operands[0];
			out[1] = id;
			memcpy(out + 2, operands + 1, (count - 1) * sizeof(*operands));
		}
		else
		{
			out[0] = id;
			memcpy(out + 1, operands, count * sizeof(*operands));
		}
		instruction(m, SECTION_GLOBALS, opcode, count + 1, out);
	}
	return id;
}

static unsigned int type0(struct module *m, unsigned int opcode)
{
	return intern(m, opcode, FALSE, 0, NULL);
}

static unsigned int type1(struct module *m, unsigned int opcode, unsigned int a)
{
	return intern(m, opcode, FALSE, 1, &a);
}

static unsigned int type2(struct module *m, unsigned int opcode, unsigned int a, unsigned int b)
{
	unsigned int operands[2];

	operands[0] = a;
	operands[1] = b;
	return intern(m, opcode, FALSE, 2, operands);
}

static unsigned int type_vector(struct module *m, unsigned int component, unsigned int count)
{
	return type2(m, OP_TYPE_VECTOR, component, count);
}

static unsigned int type_pointer(struct module *m, unsigned int storage, unsigned int type)
{
	return type2(m, OP_TYPE_POINTER, storage, type);
}

/* a sampled image of a dimension (arrayed or not), its texels float or
unsigned */
static unsigned int type_sampled_image(struct module *m, unsigned int dim, BOOL arrayed, BOOL unsigned_texels,
	BOOL sampled_image)
{
	unsigned int operands[7], image;

	operands[0] = unsigned_texels ? m->t_uint : m->t_float;
	operands[1] = dim;
	operands[2] = 0;
	operands[3] = arrayed ? 1 : 0;
	operands[4] = 0;
	operands[5] = 1;
	operands[6] = 0;
	image = intern(m, OP_TYPE_IMAGE, FALSE, 7, operands);
	return sampled_image ? type1(m, OP_TYPE_SAMPLED_IMAGE, image) : image;
}

/* a struct is never interned: each has its own decorations */
static unsigned int type_struct(struct module *m, int count, const unsigned int *members)
{
	unsigned int operands[32];
	unsigned int id = id_new(m);

	operands[0] = id;
	memcpy(operands + 1, members, count * sizeof(*members));
	instruction(m, SECTION_GLOBALS, OP_TYPE_STRUCT, count + 1, operands);
	return id;
}

static unsigned int constant_uint(struct module *m, unsigned int value)
{
	unsigned int operands[2];

	operands[0] = m->t_uint;
	operands[1] = value;
	return intern(m, OP_CONSTANT, TRUE, 2, operands);
}

static unsigned int constant_int(struct module *m, int value)
{
	unsigned int operands[2];

	operands[0] = m->t_int;
	operands[1] = (unsigned int)value;
	return intern(m, OP_CONSTANT, TRUE, 2, operands);
}

static unsigned int constant_float(struct module *m, float value)
{
	unsigned int operands[2];

	operands[0] = m->t_float;
	memcpy(&operands[1], &value, 4);
	return intern(m, OP_CONSTANT, TRUE, 2, operands);
}

static unsigned int constant_bool(struct module *m, BOOL value)
{
	unsigned int operands[1];

	operands[0] = m->t_bool;
	return intern(m, value ? OP_CONSTANT_TRUE : OP_CONSTANT_FALSE, TRUE, 1, operands);
}

/* a vector constant of count floats */
static unsigned int constant_vector(struct module *m, int count, const float *values)
{
	unsigned int operands[5];
	int index;

	operands[0] = count == 2 ? m->t_v2 : count == 3 ? m->t_v3 : m->t_v4;
	for (index = 0; index < count; index++)
		operands[1 + index] = constant_float(m, values[index]);
	return intern(m, OP_CONSTANT_COMPOSITE, TRUE, count + 1, operands);
}

static unsigned int constant_v4(struct module *m, float x, float y, float z, float w)
{
	float values[4];

	values[0] = x;
	values[1] = y;
	values[2] = z;
	values[3] = w;
	return constant_vector(m, 4, values);
}

/* a float smeared over a vector of count (1: the float) */
static unsigned int constant_splat(struct module *m, int count, float value)
{
	float values[4];

	values[0] = values[1] = values[2] = values[3] = value;
	return count == 1 ? constant_float(m, value) : constant_vector(m, count, values);
}

static unsigned int vector_type(const struct module *m, int count)
{
	return count == 1 ? m->t_float : count == 2 ? m->t_v2 : count == 3 ? m->t_v3 : m->t_v4;
}

/* ---------- instructions in the function */

/* an instruction with a result: its type, then count operands */
static unsigned int op(struct module *m, unsigned int opcode, unsigned int type, int count, const unsigned int *operands)
{
	unsigned int words[16];
	unsigned int id = id_new(m);

	words[0] = type;
	words[1] = id;
	memcpy(words + 2, operands, count * sizeof(*operands));
	instruction(m, SECTION_CODE, opcode, count + 2, words);
	return id;
}

static unsigned int op1(struct module *m, unsigned int opcode, unsigned int type, unsigned int a)
{
	return op(m, opcode, type, 1, &a);
}

static unsigned int op2(struct module *m, unsigned int opcode, unsigned int type, unsigned int a, unsigned int b)
{
	unsigned int operands[2];

	operands[0] = a;
	operands[1] = b;
	return op(m, opcode, type, 2, operands);
}

static unsigned int op3(struct module *m, unsigned int opcode, unsigned int type, unsigned int a, unsigned int b,
	unsigned int c)
{
	unsigned int operands[3];

	operands[0] = a;
	operands[1] = b;
	operands[2] = c;
	return op(m, opcode, type, 3, operands);
}

/* an instruction without a result */
static void op_void(struct module *m, unsigned int opcode, int count, const unsigned int *operands)
{
	instruction(m, SECTION_CODE, opcode, count, operands);
}

/* GLSL.std.450's */
static unsigned int ext(struct module *m, unsigned int instruction_number, unsigned int type, int count, const unsigned int *operands)
{
	unsigned int words[8];

	words[0] = m->glsl;
	words[1] = instruction_number;
	memcpy(words + 2, operands, count * sizeof(*operands));
	return op(m, OP_EXT_INST, type, count + 2, words);
}

static unsigned int ext1(struct module *m, unsigned int instruction_number, unsigned int type, unsigned int a)
{
	return ext(m, instruction_number, type, 1, &a);
}

static unsigned int ext2(struct module *m, unsigned int instruction_number, unsigned int type, unsigned int a, unsigned int b)
{
	unsigned int operands[2];

	operands[0] = a;
	operands[1] = b;
	return ext(m, instruction_number, type, 2, operands);
}

static unsigned int ext3(struct module *m, unsigned int instruction_number, unsigned int type, unsigned int a, unsigned int b,
	unsigned int c)
{
	unsigned int operands[3];

	operands[0] = a;
	operands[1] = b;
	operands[2] = c;
	return ext(m, instruction_number, type, 3, operands);
}

static unsigned int extract(struct module *m, unsigned int vector, unsigned int component)
{
	return op2(m, OP_COMPOSITE_EXTRACT, m->t_float, vector, component);
}

/* components of a vector (or of two: 4 on are the second's) */
static unsigned int shuffle(struct module *m, int count, unsigned int a, unsigned int b, const unsigned int *components)
{
	unsigned int operands[6];

	operands[0] = a;
	operands[1] = b;
	memcpy(operands + 2, components, count * sizeof(*components));
	return op(m, OP_VECTOR_SHUFFLE, vector_type(m, count), count + 2, operands);
}

/* .xyz, .xy of a vec4 */
static unsigned int xyz(struct module *m, unsigned int vector)
{
	static const unsigned int components[3] = { 0, 1, 2 };

	return shuffle(m, 3, vector, vector, components);
}

static unsigned int xy(struct module *m, unsigned int vector)
{
	static const unsigned int components[2] = { 0, 1 };

	return shuffle(m, 2, vector, vector, components);
}

static unsigned int construct(struct module *m, int count, const unsigned int *components)
{
	return op(m, OP_COMPOSITE_CONSTRUCT, vector_type(m, count), count, components);
}

static unsigned int construct4(struct module *m, unsigned int x, unsigned int y, unsigned int z, unsigned int w)
{
	unsigned int components[4];

	components[0] = x;
	components[1] = y;
	components[2] = z;
	components[3] = w;
	return construct(m, 4, components);
}

/* a scalar smeared over count components, as GLSL's vecN(x) */
static unsigned int splat(struct module *m, int count, unsigned int scalar)
{
	unsigned int components[4];

	if (count == 1)
		return scalar;
	components[0] = components[1] = components[2] = components[3] = scalar;
	return construct(m, count, components);
}

/* a vec4 of a vec3 and a float */
static unsigned int vec4_of(struct module *m, unsigned int vector3, unsigned int w)
{
	unsigned int components[2];

	components[0] = vector3;
	components[1] = w;
	return op(m, OP_COMPOSITE_CONSTRUCT, m->t_v4, 2, components);
}

/* a chosen by a scalar condition, else b, of count components (SPIR-V 1.3's
OpSelect takes a condition of as many components as its result) */
static unsigned int select_n(struct module *m, int count, unsigned int condition, unsigned int a, unsigned int b)
{
	unsigned int type = vector_type(m, count);

	if (count > 1)
	{
		unsigned int conditions[4];

		conditions[0] = conditions[1] = conditions[2] = conditions[3] = condition;
		condition = op(m, OP_COMPOSITE_CONSTRUCT, count == 2 ? m->t_bv2 : count == 3 ? m->t_bv3 : m->t_bv4, count, conditions);
	}
	return op3(m, OP_SELECT, type, condition, a, b);
}

static unsigned int fadd(struct module *m, int count, unsigned int a, unsigned int b)
{
	return op2(m, OP_F_ADD, vector_type(m, count), a, b);
}

static unsigned int fsub(struct module *m, int count, unsigned int a, unsigned int b)
{
	return op2(m, OP_F_SUB, vector_type(m, count), a, b);
}

static unsigned int fmul(struct module *m, int count, unsigned int a, unsigned int b)
{
	return op2(m, OP_F_MUL, vector_type(m, count), a, b);
}

static unsigned int fdiv(struct module *m, int count, unsigned int a, unsigned int b)
{
	return op2(m, OP_F_DIV, vector_type(m, count), a, b);
}

/* a vector times a float (GLSL's vec * float, scalar first or not) */
static unsigned int fscale(struct module *m, int count, unsigned int vector, unsigned int scalar)
{
	return count == 1 ? fmul(m, 1, vector, scalar) : op2(m, OP_VECTOR_TIMES_SCALAR, vector_type(m, count), vector, scalar);
}

static unsigned int fneg(struct module *m, int count, unsigned int a)
{
	return op1(m, OP_F_NEGATE, vector_type(m, count), a);
}

static unsigned int fdot(struct module *m, unsigned int a, unsigned int b)
{
	return op2(m, OP_DOT, m->t_float, a, b);
}

static unsigned int fclamp(struct module *m, int count, unsigned int x, float low, float high)
{
	return ext3(m, GLSL_F_CLAMP, vector_type(m, count), x, constant_splat(m, count, low), constant_splat(m, count, high));
}

/* a vec4 with each channel that is not a number, or is infinite, made 0:
the NV2A computed none, and a GPU draws one as anything (black, white,
blocks). By its bits, which no driver's fast arithmetic takes to be
finite. */
static unsigned int finite4(struct module *m, unsigned int v)
{
	unsigned int exponent = constant_uint(m, 0x7f800000u);
	unsigned int mask = intern(m, OP_CONSTANT_COMPOSITE, TRUE, 5,
		(const unsigned int[]){ m->t_uv4, exponent, exponent, exponent, exponent });
	unsigned int bits = op2(m, OP_BITWISE_AND, m->t_uv4, op1(m, OP_BITCAST, m->t_uv4, v), mask);

	return op3(m, OP_SELECT, m->t_v4, op2(m, OP_I_EQUAL, m->t_bv4, bits, mask), constant_splat(m, 4, 0.0f), v);
}

static unsigned int saturate(struct module *m, int count, unsigned int x)
{
	return fclamp(m, count, x, 0.0f, 1.0f);
}

static unsigned int fmax_scalar(struct module *m, int count, unsigned int x, float value)
{
	return ext2(m, GLSL_F_MAX, vector_type(m, count), x, constant_splat(m, count, value));
}

/* comparisons of floats: a bool */
static unsigned int compare(struct module *m, unsigned int opcode, unsigned int a, unsigned int b)
{
	return op2(m, opcode, m->t_bool, a, b);
}

/* ---------- blocks */

static unsigned int label(struct module *m)
{
	unsigned int id = id_new(m);

	op_void(m, OP_LABEL, 1, &id);
	return id;
}

static void label_at(struct module *m, unsigned int id)
{
	op_void(m, OP_LABEL, 1, &id);
}

static void branch(struct module *m, unsigned int target)
{
	op_void(m, OP_BRANCH, 1, &target);
}

static void selection_merge(struct module *m, unsigned int merge)
{
	unsigned int operands[2];

	operands[0] = merge;
	operands[1] = 0;
	op_void(m, OP_SELECTION_MERGE, 2, operands);
}

static void branch_conditional(struct module *m, unsigned int condition, unsigned int yes, unsigned int no)
{
	unsigned int operands[3];

	operands[0] = condition;
	operands[1] = yes;
	operands[2] = no;
	op_void(m, OP_BRANCH_CONDITIONAL, 3, operands);
}

/* if (condition) discard; as glslang writes it */
static void discard_if(struct module *m, unsigned int condition)
{
	unsigned int kill = id_new(m), merge = id_new(m);

	selection_merge(m, merge);
	branch_conditional(m, condition, kill, merge);
	label_at(m, kill);
	op_void(m, OP_KILL, 0, NULL);
	label_at(m, merge);
}

/* ---------- declarations */

static void name(struct module *m, unsigned int id, const char *string)
{
	instruction_string(m, SECTION_NAMES, OP_NAME, 1, &id, string, 0, NULL);
}

static void decorate(struct module *m, unsigned int id, unsigned int decoration, int count, const unsigned int *values)
{
	unsigned int operands[4];

	operands[0] = id;
	operands[1] = decoration;
	memcpy(operands + 2, values, count * sizeof(*values));
	instruction(m, SECTION_DECORATIONS, OP_DECORATE, count + 2, operands);
}

static void decorate1(struct module *m, unsigned int id, unsigned int decoration, unsigned int value)
{
	decorate(m, id, decoration, 1, &value);
}

static void member_decorate(struct module *m, unsigned int id, unsigned int member, unsigned int decoration, unsigned int value)
{
	unsigned int operands[4];

	operands[0] = id;
	operands[1] = member;
	operands[2] = decoration;
	operands[3] = value;
	instruction(m, SECTION_DECORATIONS, OP_MEMBER_DECORATE, 4, operands);
}

/* a module-level variable of a type in a storage class */
static unsigned int variable(struct module *m, unsigned int type, unsigned int storage, const char *variable_name)
{
	unsigned int operands[3];
	unsigned int id = id_new(m);

	operands[0] = type_pointer(m, storage, type);
	operands[1] = id;
	operands[2] = storage;
	instruction(m, SECTION_VARIABLES, OP_VARIABLE, 3, operands);
	if (variable_name)
		name(m, id, variable_name);
	if ((storage == STORAGE_INPUT || storage == STORAGE_OUTPUT) && m->interface_count < 64)
		m->interface[m->interface_count++] = id;
	return id;
}

/* an Input or Output at a location */
static unsigned int interface_variable(struct module *m, unsigned int type, unsigned int storage, unsigned int location,
	const char *variable_name)
{
	unsigned int id = variable(m, type, storage, variable_name);

	decorate1(m, id, DECORATION_LOCATION, location);
	return id;
}

static unsigned int load(struct module *m, unsigned int type, unsigned int pointer)
{
	return op1(m, OP_LOAD, type, pointer);
}

static void store(struct module *m, unsigned int pointer, unsigned int value)
{
	unsigned int operands[2];

	operands[0] = pointer;
	operands[1] = value;
	op_void(m, OP_STORE, 2, operands);
}

/* a function's variable (Function storage: declared at the entry block's
start, which is where SECTION_VARIABLES' function part goes) */
static unsigned int local(struct module *m, struct words *locals, unsigned int type)
{
	unsigned int id = id_new(m);

	words_push(m, locals, OP_VARIABLE | (4u << 16));
	words_push(m, locals, type_pointer(m, STORAGE_FUNCTION, type));
	words_push(m, locals, id);
	words_push(m, locals, STORAGE_FUNCTION);
	return id;
}

/* ---------- the uniform blocks */

/* the vertex constants: a block of 192 vec4 */
static void declare_constants(struct module *m)
{
	unsigned int length = constant_uint(m, m->motion ? 2 * XGPU_VERTEX_CONSTANT_COUNT : XGPU_VERTEX_CONSTANT_COUNT);
	unsigned int array, block;

	array = id_new(m);
	{
		unsigned int operands[3];

		operands[0] = array;
		operands[1] = m->t_v4;
		operands[2] = length;
		instruction(m, SECTION_GLOBALS, OP_TYPE_ARRAY, 3, operands);
	}
	decorate1(m, array, DECORATION_ARRAY_STRIDE, 16);
	block = type_struct(m, 1, &array);
	decorate(m, block, DECORATION_BLOCK, 0, NULL);
	member_decorate(m, block, 0, DECORATION_OFFSET, 0);
	name(m, block, "xgpu_constants");
	m->constants = variable(m, block, STORAGE_UNIFORM, "c");
	decorate1(m, m->constants, DECORATION_DESCRIPTOR_SET, XGPU_SPIRV_SET_DRAW);
	decorate1(m, m->constants, DECORATION_BINDING, XGPU_SPIRV_BINDING_CONSTANTS);
}

/* an array type of count vec4 in a block (std140: a stride of 16) */
static unsigned int block_array(struct module *m, unsigned int count)
{
	unsigned int operands[3];
	unsigned int array = id_new(m);

	operands[0] = array;
	operands[1] = m->t_v4;
	operands[2] = constant_uint(m, count);
	instruction(m, SECTION_GLOBALS, OP_TYPE_ARRAY, 3, operands);
	decorate1(m, array, DECORATION_ARRAY_STRIDE, 16);
	return array;
}

/* struct xgpu_spirv_uniforms' members, in order */
enum
{
	U_VIEWPORT_SCALE, U_VIEWPORT_OFFSET, U_POINT_SIZE, U_SCREEN_OFFSET, U_ALPHA_REFERENCE, U_SCREEN_POINTS,
	U_PS_C0, U_PS_C1, U_PS_FINAL_C0, U_PS_FINAL_C1, U_FOG_COLOR, U_FOG_PARAMETERS, U_BUMP_MATRIX, U_BUMP_LUMINANCE,
	U_TEXTURE_SCALE, U_BLEND_CONSTANT, U_TEXTURE_BORDER, U_TEXTURE_BORDER_LINEAR, U_MODEL_LIGHTS,
	U_EYE_CORRECTION, U_SCREEN_CORRECTION, U_FOVEA_VIEW, U_FOVEA_SAMPLE, U_VISIBILITY_COUNTER, U_DEPTH_BIAS,
	U_EFFECT_UNPROJECT, U_EFFECT_EYE,
	U_COUNT,
	/* (display.ambient_occlusion "world": past the others, declared by the
	pixel shaders that read it alone: declare_uniforms_world; and past it the
	enhanced dynamic lights' tint, by the structure's lit by them) */
	U_WORLD_AO = U_COUNT,
	U_DYNAMIC_LIGHT_TINT,
};

static void declare_uniforms_members(struct module *m, int count)
{
	static const unsigned long offsets[U_COUNT + 2] =
	{
		offsetof(struct xgpu_spirv_uniforms, viewport_scale), offsetof(struct xgpu_spirv_uniforms, viewport_offset),
		offsetof(struct xgpu_spirv_uniforms, point_size), offsetof(struct xgpu_spirv_uniforms, screen_offset),
		offsetof(struct xgpu_spirv_uniforms, alpha_reference), offsetof(struct xgpu_spirv_uniforms, screen_points),
		offsetof(struct xgpu_spirv_uniforms, ps_c0), offsetof(struct xgpu_spirv_uniforms, ps_c1),
		offsetof(struct xgpu_spirv_uniforms, ps_final_c0), offsetof(struct xgpu_spirv_uniforms, ps_final_c1),
		offsetof(struct xgpu_spirv_uniforms, fog_color), offsetof(struct xgpu_spirv_uniforms, fog_parameters),
		offsetof(struct xgpu_spirv_uniforms, bump_matrix), offsetof(struct xgpu_spirv_uniforms, bump_luminance),
		offsetof(struct xgpu_spirv_uniforms, texture_scale), offsetof(struct xgpu_spirv_uniforms, blend_constant),
		offsetof(struct xgpu_spirv_uniforms, texture_border), offsetof(struct xgpu_spirv_uniforms, texture_border_linear),
		offsetof(struct xgpu_spirv_uniforms, model_lights),
		offsetof(struct xgpu_spirv_uniforms, eye_correction), offsetof(struct xgpu_spirv_uniforms, screen_correction),
		offsetof(struct xgpu_spirv_uniforms, fovea_view), offsetof(struct xgpu_spirv_uniforms, fovea_sample),
		offsetof(struct xgpu_spirv_uniforms, visibility_counter), offsetof(struct xgpu_spirv_uniforms, depth_bias),
		offsetof(struct xgpu_spirv_uniforms, effect_unproject), offsetof(struct xgpu_spirv_uniforms, effect_eye),
		offsetof(struct xgpu_spirv_uniforms, world_ao), offsetof(struct xgpu_spirv_uniforms, dynamic_light_tint),
	};
	unsigned int members[U_COUNT + 2], block;
	int member;

	for (member = 0; member < count; member++)
	{
		switch (member)
		{
		case U_POINT_SIZE: case U_SCREEN_OFFSET: case U_ALPHA_REFERENCE: case U_SCREEN_POINTS: case U_VISIBILITY_COUNTER:
		case U_DEPTH_BIAS:
			members[member] = m->t_float;
			break;
		case U_PS_C0: case U_PS_C1: case U_EYE_CORRECTION: members[member] = block_array(m, 8); break;
		case U_BUMP_MATRIX: case U_BUMP_LUMINANCE: case U_TEXTURE_SCALE: case U_TEXTURE_BORDER:
		case U_FOVEA_VIEW: case U_FOVEA_SAMPLE: case U_EFFECT_EYE:
			members[member] = block_array(m, 4);
			break;
		case U_EFFECT_UNPROJECT: members[member] = block_array(m, 16); break;
		case U_SCREEN_CORRECTION: members[member] = block_array(m, 2); break;
		case U_MODEL_LIGHTS: members[member] = block_array(m, XGPU_MODEL_LIGHT_COUNT); break;
		default: members[member] = m->t_v4; break;
		}
	}
	block = type_struct(m, count, members);
	decorate(m, block, DECORATION_BLOCK, 0, NULL);
	for (member = 0; member < count; member++)
		member_decorate(m, block, member, DECORATION_OFFSET, (unsigned int)offsets[member]);
	name(m, block, "xgpu_uniforms");
	m->uniforms = variable(m, block, STORAGE_UNIFORM, "u");
	decorate1(m, m->uniforms, DECORATION_DESCRIPTOR_SET, XGPU_SPIRV_SET_DRAW);
	decorate1(m, m->uniforms, DECORATION_BINDING, XGPU_SPIRV_BINDING_UNIFORMS);
}

static void declare_uniforms(struct module *m)
{
	declare_uniforms_members(m, U_COUNT);
}

/* (and the draw's object's, U_WORLD_AO: display.ambient_occlusion "world";
and past it, U_DYNAMIC_LIGHT_TINT, the enhanced dynamic lights' tint: a
structure's pass lit by them) */
static void declare_uniforms_world(struct module *m, BOOL tint)
{
	declare_uniforms_members(m, tint ? U_COUNT + 2 : U_COUNT + 1);
}

/* a member of the uniforms (an element of an array member): a float or a
vec4 */
static unsigned int uniform(struct module *m, int member, int element)
{
	unsigned int operands[3], pointer;
	BOOL scalar = (member >= U_POINT_SIZE && member <= U_SCREEN_POINTS) || member == U_VISIBILITY_COUNTER ||
		member == U_DEPTH_BIAS;
	unsigned int type = scalar ? m->t_float : m->t_v4;
	int count = 2;

	operands[0] = m->uniforms;
	operands[1] = constant_int(m, member);
	if (element >= 0)
		operands[count++] = constant_int(m, element);
	pointer = op(m, OP_ACCESS_CHAIN, type_pointer(m, STORAGE_UNIFORM, type), count, operands);
	return load(m, type, pointer);
}

/* a vec4 array member indexed by an int value */
static unsigned int uniform_index(struct module *m, int member, unsigned int index)
{
	unsigned int operands[3], pointer;

	operands[0] = m->uniforms;
	operands[1] = constant_int(m, member);
	operands[2] = index;
	pointer = op(m, OP_ACCESS_CHAIN, type_pointer(m, STORAGE_UNIFORM, m->t_v4), 3, operands);
	return load(m, m->t_v4, pointer);
}

/* ViewIndex is relative to the attachment view's first layer, as GL's
ViewID is. The one-eye foveated pass has two views of its own. */
static void declare_views(struct module *m, int multiview, BOOL foveated, BOOL fragment)
{
	unsigned int capability = CAPABILITY_MULTIVIEW;

	if (multiview != XGPU_MULTIVIEW_EYES && !(multiview && foveated))
		return;
	instruction(m, SECTION_CAPABILITIES, OP_CAPABILITY, 1, &capability);
	m->view_index = variable(m, m->t_int, STORAGE_INPUT, "gl_ViewIndex");
	decorate1(m, m->view_index, DECORATION_BUILT_IN, BUILT_IN_VIEW_INDEX);
	if (fragment)
		decorate(m, m->view_index, DECORATION_FLAT, 0, NULL);
}

static unsigned int view_number(struct module *m, int multiview, BOOL foveated)
{
	unsigned int view;

	if (!multiview)
		return constant_int(m, 0);
	if (!foveated)
		return multiview == XGPU_MULTIVIEW_EYES ? load(m, m->t_int, m->view_index) :
			constant_int(m, multiview == XGPU_MULTIVIEW_RIGHT_EYE ? 1 : 0);
	view = load(m, m->t_int, m->view_index);
	if (multiview == XGPU_MULTIVIEW_RIGHT_EYE)
		view = op2(m, OP_I_ADD, m->t_int, view, constant_int(m, 2));
	return view;
}

/* eye_correction[eye] * position, from its column-major vec4 columns */
static unsigned int eye_position(struct module *m, unsigned int eye, unsigned int position)
{
	unsigned int column = op2(m, OP_I_MUL, m->t_int, eye, constant_int(m, 4));
	unsigned int result = 0;
	int component;

	for (component = 0; component < 4; component++)
	{
		unsigned int index = component ? op2(m, OP_I_ADD, m->t_int, column, constant_int(m, component)) : column;
		unsigned int part = fscale(m, 4, uniform_index(m, U_EYE_CORRECTION, index), extract(m, position, component));

		result = component ? fadd(m, 4, result, part) : part;
	}
	return result;
}

/* c[index] (an int value: dynamic, every register counted as read) */
static unsigned int vertex_constant(struct module *m, unsigned int index_id)
{
	unsigned int operands[3], pointer;

	memset(m->constants_read, 0xff, sizeof(m->constants_read));
	if (m->previous)
		index_id = op2(m, OP_I_ADD, m->t_int, index_id, constant_int(m, XGPU_VERTEX_CONSTANT_COUNT));
	operands[0] = m->constants;
	operands[1] = constant_int(m, 0);
	operands[2] = index_id;
	pointer = op(m, OP_ACCESS_CHAIN, type_pointer(m, STORAGE_UNIFORM, m->t_v4), 3, operands);
	return load(m, m->t_v4, pointer);
}

/* c[index] (a constant index: the registers read, xgpu_spirv_module's) */
static unsigned int vertex_constant_at(struct module *m, int index)
{
	unsigned int operands[3], pointer;

	if (index >= 0 && index < XGPU_VERTEX_CONSTANT_COUNT)
		m->constants_read[index >> 6] |= 1ull << (index & 63);
	operands[0] = m->constants;
	operands[1] = constant_int(m, 0);
	operands[2] = constant_int(m, m->previous ? index + XGPU_VERTEX_CONSTANT_COUNT : index);
	pointer = op(m, OP_ACCESS_CHAIN, type_pointer(m, STORAGE_UNIFORM, m->t_v4), 3, operands);
	return load(m, m->t_v4, pointer);
}

/* ---------- the module's beginning and end */

static void module_begin(struct module *m, unsigned int execution_model)
{
	static const char glsl_name[] = "GLSL.std.450";
	unsigned int operands[2];

	(void)execution_model;
	memset(m, 0, sizeof(*m));
	m->next_id = 1;
	operands[0] = CAPABILITY_SHADER;
	instruction(m, SECTION_CAPABILITIES, OP_CAPABILITY, 1, operands);
	m->glsl = id_new(m);
	instruction_string(m, SECTION_IMPORTS, OP_EXT_INST_IMPORT, 1, &m->glsl, glsl_name, 0, NULL);
	operands[0] = 0; /* Logical */
	operands[1] = 1; /* GLSL450 */
	instruction(m, SECTION_MEMORY_MODEL, OP_MEMORY_MODEL, 2, operands);
	m->t_void = type0(m, OP_TYPE_VOID);
	m->t_bool = type0(m, OP_TYPE_BOOL);
	m->t_int = type2(m, OP_TYPE_INT, 32, 1);
	m->t_uint = type2(m, OP_TYPE_INT, 32, 0);
	m->t_float = type1(m, OP_TYPE_FLOAT, 32);
	m->t_v2 = type_vector(m, m->t_float, 2);
	m->t_v3 = type_vector(m, m->t_float, 3);
	m->t_v4 = type_vector(m, m->t_float, 4);
	m->t_iv2 = type_vector(m, m->t_int, 2);
	m->t_iv3 = type_vector(m, m->t_int, 3);
	m->t_iv4 = type_vector(m, m->t_int, 4);
	m->t_uv4 = type_vector(m, m->t_uint, 4);
	m->t_uv2 = type_vector(m, m->t_uint, 2);
	m->t_bv2 = type_vector(m, m->t_bool, 2);
	m->t_bv3 = type_vector(m, m->t_bool, 3);
	m->t_bv4 = type_vector(m, m->t_bool, 4);
}

/* the function "main" begun: its locals go before its first block's
instructions, which are written apart until the end */
static unsigned int function_begin(struct module *m)
{
	unsigned int operands[4];
	unsigned int main_id = id_new(m);

	operands[0] = m->t_void;
	operands[1] = main_id;
	operands[2] = 0;
	operands[3] = type1(m, OP_TYPE_FUNCTION, m->t_void);
	instruction(m, SECTION_CODE, OP_FUNCTION, 4, operands);
	label(m);
	return main_id;
}

static void module_free(struct module *m)
{
	int section;

	for (section = 0; section < SECTION_COUNT; section++)
		free(m->sections[section].words);
	free(m->keys.words);
	free(m->interned);
}

/* the words laid out: the header, the sections, the function (its locals
after its first label) */
static BOOL module_end(struct module *m, unsigned int execution_model, unsigned int main_id, const struct words *locals,
	struct xgpu_spirv_module *out)
{
	unsigned long total = 5, at, index;
	int section;
	unsigned int operands[2];

	op_void(m, OP_RETURN, 0, NULL);
	instruction(m, SECTION_CODE, OP_FUNCTION_END, 0, NULL);
	if (m->image_query)
	{
		operands[0] = CAPABILITY_IMAGE_QUERY;
		instruction(m, SECTION_CAPABILITIES, OP_CAPABILITY, 1, operands);
	}
	if (m->group_non_uniform)
	{
		operands[0] = CAPABILITY_GROUP_NON_UNIFORM;
		instruction(m, SECTION_CAPABILITIES, OP_CAPABILITY, 1, operands);
		operands[0] = CAPABILITY_GROUP_NON_UNIFORM_ARITHMETIC;
		instruction(m, SECTION_CAPABILITIES, OP_CAPABILITY, 1, operands);
	}
	if (m->ray_query)
	{
		operands[0] = CAPABILITY_RAY_QUERY;
		instruction(m, SECTION_CAPABILITIES, OP_CAPABILITY, 1, operands);
		instruction_string(m, SECTION_EXTENSIONS, OP_EXTENSION, 0, NULL, "SPV_KHR_ray_query", 0, NULL);
	}
	if (m->rt_hit_textures)
	{
		operands[0] = CAPABILITY_SHADER_NON_UNIFORM;
		instruction(m, SECTION_CAPABILITIES, OP_CAPABILITY, 1, operands);
		operands[0] = CAPABILITY_SAMPLED_IMAGE_ARRAY_NON_UNIFORM_INDEXING;
		instruction(m, SECTION_CAPABILITIES, OP_CAPABILITY, 1, operands);
		instruction_string(m, SECTION_EXTENSIONS, OP_EXTENSION, 0, NULL, "SPV_EXT_descriptor_indexing", 0, NULL);
	}
	{
		unsigned int words[68];

		words[0] = execution_model;
		words[1] = main_id;
		instruction_string(m, SECTION_ENTRY_POINTS, OP_ENTRY_POINT, 2, words, "main", m->interface_count, m->interface);
	}
	if (execution_model == EXECUTION_MODEL_FRAGMENT)
	{
		operands[0] = main_id;
		operands[1] = EXECUTION_MODE_ORIGIN_UPPER_LEFT;
		instruction(m, SECTION_EXECUTION_MODES, OP_EXECUTION_MODE, 2, operands);
	}
	if (execution_model == EXECUTION_MODEL_GL_COMPUTE)
	{
		unsigned int words[5];

		words[0] = main_id;
		words[1] = EXECUTION_MODE_LOCAL_SIZE;
		words[2] = XGPU_SPIRV_RT_SKIN_GROUP;
		words[3] = 1;
		words[4] = 1;
		instruction(m, SECTION_EXECUTION_MODES, OP_EXECUTION_MODE, 5, words);
	}
	if (m->failed)
	{
		module_free(m);
		return FALSE;
	}
	for (section = 0; section < SECTION_COUNT; section++)
		total += m->sections[section].count;
	total += locals->count;
	out->words = malloc(total * sizeof(*out->words));
	if (!out->words)
	{
		module_free(m);
		return FALSE;
	}
	memcpy(out->constants_read, m->constants_read, sizeof(out->constants_read));
	out->words[0] = SPV_MAGIC;
	out->words[1] = SPV_VERSION;
	out->words[2] = 0;
	out->words[3] = m->next_id;
	out->words[4] = 0;
	at = 5;
	for (section = 0; section < SECTION_COUNT; section++)
	{
		const struct words *words = &m->sections[section];

		if (section == SECTION_CODE)
		{
			/* OpFunction (5 words), the first OpLabel (2), the locals, the rest */
			memcpy(out->words + at, words->words, 7 * sizeof(*words->words));
			at += 7;
			if (locals->count)
				memcpy(out->words + at, locals->words, locals->count * sizeof(*locals->words));
			at += locals->count;
			for (index = 7; index < words->count; index++)
				out->words[at++] = words->words[index];
		}
		else if (words->count)
		{
			memcpy(out->words + at, words->words, words->count * sizeof(*words->words));
			at += words->count;
		}
	}
	out->count = at;
	module_free(m);
	return TRUE;
}

/* ---------- the position's invariance

GLSL's invariant gl_Position makes the GL driver compute every operation
the position depends on as written (NVIDIA's: each one "precise", none fused
into a multiply-add), so that the passes drawing the same surface meet
exactly. SPIR-V's Invariant decoration does not say how; the operations the
position's store depends on are decorated NoContraction, as glslang does
for GLSL's precise, so that both drivers compute it alike. */

/* the operands of an instruction of the code that are ids (the others are
literals): bit n, the operand after the result (or after the opcode, for an
instruction without one); and whether it has a result */
static unsigned int id_operands(unsigned int opcode, unsigned int count, BOOL *has_result)
{
	*has_result = TRUE;
	switch (opcode)
	{
	case OP_VECTOR_SHUFFLE: return 3;
	case OP_COMPOSITE_EXTRACT: return 1;
	case OP_COMPOSITE_INSERT: return 3;
	case OP_EXT_INST: return ~0u << 2 | 1;
	case OP_IMAGE_FETCH: return ~0u << 3 | 3;
	case OP_STORE: *has_result = FALSE; return 3;
	case OP_LABEL: return 0;
	case OP_BRANCH: case OP_BRANCH_CONDITIONAL: case OP_SELECTION_MERGE: case OP_LOOP_MERGE: case OP_KILL: case OP_RETURN:
	case OP_FUNCTION_END:
		*has_result = FALSE;
		return 0;
	default:
		(void)count;
		return ~0u;
	}
}

static BOOL contractible(unsigned int opcode)
{
	switch (opcode)
	{
	case OP_F_ADD: case OP_F_SUB: case OP_F_MUL: case OP_F_DIV: case OP_VECTOR_TIMES_SCALAR: case OP_DOT: case OP_F_NEGATE:
		return TRUE;
	default:
		return FALSE;
	}
}

/* the arithmetic the store of value into a variable depends on, decorated
NoContraction */
static void precise_dependencies(struct module *m, unsigned int variable_id)
{
	const struct words *code = &m->sections[SECTION_CODE];
	unsigned long *defined, at;
	unsigned char *needed;
	unsigned int *stack, stack_count = 0, id;

	defined = calloc(m->next_id, sizeof(*defined));
	needed = calloc(m->next_id, 1);
	stack = malloc(m->next_id * sizeof(*stack));
	if (!defined || !needed || !stack)
	{
		m->failed = TRUE;
		free(defined);
		free(needed);
		free(stack);
		return;
	}
	/* (where each result is defined; the value stored, the start) */
	for (at = 0; at < code->count; at += code->words[at] >> 16)
	{
		unsigned int opcode = code->words[at] & 0xffff, length = code->words[at] >> 16;
		BOOL has_result;

		if (!length)
			break;
		id_operands(opcode, length, &has_result);
		if (opcode == OP_STORE && code->words[at + 1] == variable_id && code->words[at + 2] < m->next_id &&
			!needed[code->words[at + 2]])
		{
			needed[code->words[at + 2]] = 1;
			stack[stack_count++] = code->words[at + 2];
		}
		else if (has_result && opcode != OP_FUNCTION && opcode != OP_LABEL && length >= 3 && code->words[at + 2] < m->next_id)
		{
			defined[code->words[at + 2]] = at + 1;
		}
	}
	while (stack_count)
	{
		unsigned long where;
		unsigned int opcode, length, ids, operand;

		id = stack[--stack_count];
		where = defined[id];
		if (!where)
			continue;
		where--;
		opcode = code->words[where] & 0xffff;
		length = code->words[where] >> 16;
		if (contractible(opcode))
			decorate(m, id, DECORATION_NO_CONTRACTION, 0, NULL);
		{
			BOOL has_result;

			ids = id_operands(opcode, length, &has_result);
		}
		for (operand = 3; operand < length; operand++)
		{
			unsigned int word = code->words[where + operand];

			if (operand - 3 < 32 && !(ids & (1u << (operand - 3))))
				continue;
			if (word < m->next_id && !needed[word])
			{
				needed[word] = 1;
				stack[stack_count++] = word;
			}
		}
	}
	free(defined);
	free(needed);
	free(stack);
}

/* ---------- vertex programs */

/* where an output register's value goes (else none: the back faces'
colors, which no pixel shader reads, and the unused) */
enum
{
	OUT_POSITION, OUT_D0, OUT_D1, OUT_FOG, OUT_PTS, OUT_T0, OUT_T1, OUT_T2, OUT_T3, OUT_COUNT,
};

static int output_slot(unsigned char output)
{
	switch (output)
	{
	case XGPU_OUTPUT_POSITION: return OUT_POSITION;
	case XGPU_OUTPUT_DIFFUSE: return OUT_D0;
	case XGPU_OUTPUT_SPECULAR: return OUT_D1;
	case XGPU_OUTPUT_FOG: return OUT_FOG;
	case XGPU_OUTPUT_POINT_SIZE: return OUT_PTS;
	case XGPU_OUTPUT_TEXTURE0: return OUT_T0;
	case XGPU_OUTPUT_TEXTURE1: return OUT_T1;
	case XGPU_OUTPUT_TEXTURE2: return OUT_T2;
	case XGPU_OUTPUT_TEXTURE3: return OUT_T3;
	default: return -1;
	}
}

/* a register's components written from a value's (mask: bit 3 x ... bit 0 w) */
static unsigned int write_masked(struct module *m, unsigned int old, unsigned int value, unsigned long mask)
{
	unsigned int components[4];
	int component;

	if ((mask & 15) == 15)
		return value;
	for (component = 0; component < 4; component++)
		components[component] = (mask & (8 >> component)) ? 4 + component : component;
	return shuffle(m, 4, old, value, components);
}

/* the program's registers, as SSA values */
struct vertex_state
{
	unsigned int r[12], position, outputs[OUT_COUNT];
	unsigned int inputs[XGPU_VERTEX_ATTRIBUTE_COUNT];
	unsigned int a0;
	unsigned int mac, ilu;
	unsigned int zero;
};

static unsigned int vertex_operand(struct module *m, struct vertex_state *state, const struct xgpu_vertex_operand *operand,
	int relative)
{
	unsigned int value, components[4];
	int component;

	switch (operand->file)
	{
	case XGPU_VERTEX_TEMPORARY:
		/* r12 reads back the position output; past it there are none */
		if (operand->index == XGPU_VERTEX_POSITION)
			value = state->position;
		else if (operand->index < XGPU_VERTEX_POSITION)
			value = state->r[operand->index];
		else
			value = state->zero;
		break;
	case XGPU_VERTEX_INPUT:
		value = operand->index < XGPU_VERTEX_ATTRIBUTE_COUNT ? state->inputs[operand->index] : state->zero;
		break;
	case XGPU_VERTEX_CONSTANT:
		if (relative)
		{
			unsigned int sum = op2(m, OP_I_ADD, m->t_int, state->a0, constant_int(m, operand->index));

			value = vertex_constant(m, ext3(m, GLSL_S_CLAMP, m->t_int, sum, constant_int(m, 0),
				constant_int(m, XGPU_VERTEX_CONSTANT_COUNT - 1)));
		}
		else
		{
			value = vertex_constant_at(m, operand->index);
		}
		break;
	default:
		value = state->zero;
		break;
	}
	for (component = 0; component < 4; component++)
		components[component] = operand->swizzle[component] & 3;
	if (components[0] != 0 || components[1] != 1 || components[2] != 2 || components[3] != 3)
		value = shuffle(m, 4, value, value, components);
	if (operand->negate)
		value = fneg(m, 4, value);
	return value;
}

/* the operands a unit's operation reads: bit 0 A, 1 B, 2 C */
static unsigned int mac_operands(unsigned char mac)
{
	switch (mac)
	{
	case XGPU_MAC_MOV: case XGPU_MAC_ARL: return 1;
	case XGPU_MAC_ADD: return 5;
	case XGPU_MAC_MAD: return 7;
	case XGPU_MAC_MUL: case XGPU_MAC_DP3: case XGPU_MAC_DPH: case XGPU_MAC_DP4: case XGPU_MAC_DST:
	case XGPU_MAC_MIN: case XGPU_MAC_MAX: case XGPU_MAC_SLT: case XGPU_MAC_SGE:
		return 3;
	default: return 0;
	}
}

/* vec4(bvec4): 1.0 where true */
static unsigned int bools_to_floats(struct module *m, unsigned int bools)
{
	return op3(m, OP_SELECT, m->t_v4, bools, constant_splat(m, 4, 1.0f), constant_splat(m, 4, 0.0f));
}

static unsigned int mac_operation(struct module *m, struct vertex_state *state, unsigned char mac, unsigned int a,
	unsigned int b, unsigned int c)
{
	switch (mac)
	{
	case XGPU_MAC_MOV: case XGPU_MAC_ARL: return a;
	case XGPU_MAC_MUL: return fmul(m, 4, a, b);
	case XGPU_MAC_ADD: return fadd(m, 4, a, c);
	case XGPU_MAC_MAD: return fadd(m, 4, fmul(m, 4, a, b), c);
	case XGPU_MAC_DP3: return splat(m, 4, fdot(m, xyz(m, a), xyz(m, b)));
	case XGPU_MAC_DPH: return splat(m, 4, fadd(m, 1, fdot(m, xyz(m, a), xyz(m, b)), extract(m, b, 3)));
	case XGPU_MAC_DP4: return splat(m, 4, fdot(m, a, b));
	case XGPU_MAC_DST:
		return construct4(m, constant_float(m, 1.0f), fmul(m, 1, extract(m, a, 1), extract(m, b, 1)), extract(m, a, 2),
			extract(m, b, 3));
	case XGPU_MAC_MIN: return ext2(m, GLSL_F_MIN, m->t_v4, a, b);
	case XGPU_MAC_MAX: return ext2(m, GLSL_F_MAX, m->t_v4, a, b);
	case XGPU_MAC_SLT: return bools_to_floats(m, op2(m, OP_F_ORD_LESS_THAN, m->t_bv4, a, b));
	case XGPU_MAC_SGE: return bools_to_floats(m, op2(m, OP_F_ORD_GREATER_THAN_EQUAL, m->t_bv4, a, b));
	default: return state->zero;
	}
}

static unsigned int ilu_operation(struct module *m, struct vertex_state *state, unsigned char ilu, unsigned int c)
{
	unsigned int x, r, e;

	if (ilu == XGPU_ILU_MOV)
		return c;
	if (ilu == XGPU_ILU_LIT)
	{
		/* vec4(1.0, max(s.x, 0.0), s.x > 0.0 ? pow(max(s.y, 0.0), clamp(s.w, ...)) : 0.0, 1.0) */
		unsigned int sx = extract(m, c, 0), sy = extract(m, c, 1), sw = extract(m, c, 3);
		unsigned int specular = ext2(m, GLSL_POW, m->t_float, fmax_scalar(m, 1, sy, 0.0f),
			fclamp(m, 1, sw, (float)-127.9961, (float)127.9961));

		specular = op3(m, OP_SELECT, m->t_float, compare(m, OP_F_ORD_GREATER_THAN, sx, constant_float(m, 0.0f)), specular,
			constant_float(m, 0.0f));
		return construct4(m, constant_float(m, 1.0f), fmax_scalar(m, 1, sx, 0.0f), specular, constant_float(m, 1.0f));
	}
	if (ilu == XGPU_ILU_NOP || ilu > XGPU_ILU_LIT)
		return state->zero;
	x = extract(m, c, 0);
	switch (ilu)
	{
	case XGPU_ILU_RCP:
		return splat(m, 4, fdiv(m, 1, constant_float(m, 1.0f), x));
	case XGPU_ILU_RCC:
		/* (nv2a_rcc) */
		r = fdiv(m, 1, constant_float(m, 1.0f), x);
		r = op3(m, OP_SELECT, m->t_float, compare(m, OP_F_ORD_GREATER_THAN, r, constant_float(m, 0.0f)),
			fclamp(m, 1, r, (float)5.42101e-20, (float)1.884467e+19), fclamp(m, 1, r, (float)-1.884467e+19, (float)-5.42101e-20));
		return splat(m, 4, r);
	case XGPU_ILU_RSQ:
		return splat(m, 4, ext1(m, GLSL_INVERSE_SQRT, m->t_float, ext1(m, GLSL_F_ABS, m->t_float, x)));
	case XGPU_ILU_EXP:
		return construct4(m, ext1(m, GLSL_EXP2, m->t_float, ext1(m, GLSL_FLOOR, m->t_float, x)),
			ext1(m, GLSL_FRACT, m->t_float, x), ext1(m, GLSL_EXP2, m->t_float, x), constant_float(m, 1.0f));
	case XGPU_ILU_LOG:
		/* (nv2a_log: zero's answer, else the exponent, the mantissa, the log) */
		x = ext1(m, GLSL_F_ABS, m->t_float, x);
		e = ext1(m, GLSL_FLOOR, m->t_float, ext1(m, GLSL_LOG2, m->t_float, x));
		r = construct4(m, e, fdiv(m, 1, x, ext1(m, GLSL_EXP2, m->t_float, e)), ext1(m, GLSL_LOG2, m->t_float, x),
			constant_float(m, 1.0f));
		return select_n(m, 4, compare(m, OP_F_ORD_EQUAL, x, constant_float(m, 0.0f)),
			constant_v4(m, (float)-1.0e30, 1.0f, (float)-1.0e30, 1.0f), r);
	default:
		return state->zero;
	}
}

/* the lighting's outputs (the pixel shader's model_lighting) */
static BOOL lighting_position(const struct nv2a_vertex_lighting *lighting)
{
	return lighting && (lighting->lights == 2 || lighting->world_found || lighting->lights == XGPU_ENVIRONMENT_LIGHTING);
}

/* a model lighting register of the vertex constants, in the per-pixel
lighting's order (xgpu.h: [0] c[-82], [1] to [11] c[-79] to c[-69]) */
static unsigned int model_light_constant(struct module *m, int index)
{
	return vertex_constant_at(m, XGPU_VERTEX_CONSTANT_BIAS + (index ? index - 80 : -82));
}

/* display.ambient_occlusion, a model lit for each vertex
(XGPU_AMBIENT_OCCLUSION_LIGHT_VERTEX): the light its program gives the
vertex (the pixel shader's model_lighting, at the vertex), its ambient and
distant lights' sum and its two point lights' apart, unclamped: the pixel
shader adds them, the first by the occlusion, and clamps the sum as the
program clamps it */
static void vertex_light_split(struct module *m, const struct nv2a_vertex_lighting *lighting,
	unsigned int world_normal, unsigned int world_position, unsigned int *ambient, unsigned int *point)
{
	unsigned int n = xyz(m, world_normal), facing;
	int index;

	facing = fdot(m, n, fneg(m, 3, xyz(m, model_light_constant(m, 7))));
	*ambient = fadd(m, 3, fadd(m, 3, xyz(m, model_light_constant(m, 11)),
		fscale(m, 3, xyz(m, model_light_constant(m, 8)), fmax_scalar(m, 1, ext2(m, GLSL_F_MAX, m->t_float, facing,
		fmul(m, 1, fneg(m, 1, facing), extract(m, model_light_constant(m, 0), 2))), 0.0f))),
		fscale(m, 3, xyz(m, model_light_constant(m, 10)), fmax_scalar(m, 1,
		fdot(m, n, fneg(m, 3, xyz(m, model_light_constant(m, 9)))), 0.0f)));
	*point = constant_splat(m, 3, 0.0f);
	for (index = 0; lighting->lights == 2 && index < 2; index++)
	{
		int first = 1 + index * 3;
		unsigned int first_light = model_light_constant(m, first), cone = model_light_constant(m, first + 1);
		unsigned int color = model_light_constant(m, first + 2);
		unsigned int to_light = fsub(m, 3, xyz(m, first_light), world_position);
		unsigned int distance_squared = fdot(m, to_light, to_light);
		unsigned int l = fscale(m, 3, to_light, ext1(m, GLSL_INVERSE_SQRT, m->t_float,
			fmax_scalar(m, 1, distance_squared, (float)1.0e-12)));
		unsigned int term;

		term = fmul(m, 1, fmul(m, 1, fmax_scalar(m, 1, fsub(m, 1, constant_float(m, 1.0f),
			fmul(m, 1, distance_squared, extract(m, first_light, 3))), 0.0f), saturate(m, 1, fdot(m, l, n))),
			saturate(m, 1, fadd(m, 1, fmul(m, 1, fdot(m, l, fneg(m, 3, xyz(m, cone))), extract(m, cone, 3)), extract(m, color, 3))));
		*point = fadd(m, 3, *point, fscale(m, 3, xyz(m, color), term));
	}
}

/* the inputs a program reads (bit n v<n>) */
static unsigned long vertex_inputs(const struct xgpu_vertex_program *program, const struct nv2a_vertex_lighting *lighting)
{
	unsigned long inputs = 0, index;

	for (index = 0; index < program->instruction_count; index++)
	{
		const struct xgpu_vertex_instruction *instruction = &program->instructions[index];
		unsigned int operands = mac_operands(instruction->mac) |
			(instruction->ilu != XGPU_ILU_NOP && instruction->ilu <= XGPU_ILU_LIT ? 4 : 0);
		const struct xgpu_vertex_operand *read[3];
		int operand;

		read[0] = &instruction->a;
		read[1] = &instruction->b;
		read[2] = &instruction->c;
		for (operand = 0; operand < 3; operand++)
		{
			if ((operands & (1 << operand)) && read[operand]->file == XGPU_VERTEX_INPUT &&
				read[operand]->index < XGPU_VERTEX_ATTRIBUTE_COUNT)
			{
				inputs |= 1UL << read[operand]->index;
			}
		}
	}
	if (lighting && lighting->lights == XGPU_ENVIRONMENT_LIGHTING)
		inputs |= 0xf;
	return inputs;
}

/* unpack_normpacked3: 11, 11 and 10 bits, signed */
static unsigned int unpack_normpacked3(struct module *m, unsigned int packed)
{
	unsigned int x, y, z;

	x = op2(m, OP_SHIFT_RIGHT_ARITHMETIC, m->t_int,
		op1(m, OP_BITCAST, m->t_int, op2(m, OP_SHIFT_LEFT_LOGICAL, m->t_uint, packed, constant_int(m, 21))), constant_int(m, 21));
	y = op2(m, OP_SHIFT_RIGHT_ARITHMETIC, m->t_int,
		op1(m, OP_BITCAST, m->t_int, op2(m, OP_SHIFT_LEFT_LOGICAL, m->t_uint, packed, constant_int(m, 10))), constant_int(m, 21));
	z = op2(m, OP_SHIFT_RIGHT_ARITHMETIC, m->t_int, op1(m, OP_BITCAST, m->t_int, packed), constant_int(m, 22));
	return construct4(m, fdiv(m, 1, op1(m, OP_CONVERT_S_TO_F, m->t_float, x), constant_float(m, 1023.0f)),
		fdiv(m, 1, op1(m, OP_CONVERT_S_TO_F, m->t_float, y), constant_float(m, 1023.0f)),
		fdiv(m, 1, op1(m, OP_CONVERT_S_TO_F, m->t_float, z), constant_float(m, 511.0f)), constant_float(m, 1.0f));
}

unsigned long xgpu_spirv_vertex_inputs(const struct xgpu_vertex_program *program, const struct nv2a_vertex_lighting *lighting)
{
	return vertex_inputs(program, lighting);
}

BOOL xgpu_spirv_vertex_world_position(const struct nv2a_vertex_lighting *lighting)
{
	return lighting_position(lighting);
}

/* ---------- display.temporal_aa's motion

The objects' draws while a window's objects draw with the motion
(xgpu_spirv_options' motion): the program run a second time on the previous
frame's registers (the 192 after the 192: the game's previous projection
and nodes where it has them, d3d8_device.c's halo_motion_previous_constants,
else the registers as they are), each position taken through the screen's
conversion as the draw's is (the same jitter and half pixel, which the
difference leaves out), and handed to the pixel shader as x, y and w: the
pixel's motion, the previous less the current, as a fraction of the view. */

/* a position as the screen's conversion has it, before the depth: x, y, w
(the window's clip position, the conversion's half pixel and the jitter in
it; no multiview) */
static unsigned int motion_screen(struct module *m, BOOL clip_captured, unsigned int clip_position,
	unsigned int position)
{
	unsigned int viewport_scale = uniform(m, U_VIEWPORT_SCALE, -1), viewport_offset = uniform(m, U_VIEWPORT_OFFSET, -1);
	unsigned int screen_offset = uniform(m, U_SCREEN_OFFSET, -1);
	unsigned int scale_parts[2], scale, shift_parts[2], shift, xy_value, w, parts[3];
	BOOL previous = m->previous;
	int component;

	m->previous = FALSE;
	for (component = 0; component < 2; component++)
	{
		unsigned int value = extract(m, viewport_scale, component);

		scale_parts[component] = op3(m, OP_SELECT, m->t_float,
			compare(m, OP_F_UNORD_NOT_EQUAL, value, constant_float(m, 0.0f)), value, constant_float(m, 1.0f));
	}
	scale = construct(m, 2, scale_parts);
	shift_parts[0] = fadd(m, 1, constant_float(m, 0.5f), screen_offset);
	shift_parts[1] = constant_float(m, 0.5f);
	shift = construct(m, 2, shift_parts);
	if (clip_captured)
	{
		w = extract(m, clip_position, 3);
		xy_value = fdiv(m, 2, fadd(m, 2, fmul(m, 2, xy(m, clip_position),
			xy(m, vertex_constant_at(m, XGPU_VERTEX_CONSTANT_BIAS - 38))),
			fscale(m, 2, fsub(m, 2, fadd(m, 2, xy(m, vertex_constant_at(m, XGPU_VERTEX_CONSTANT_BIAS - 37)), shift),
			xy(m, viewport_offset)), w)), scale);
	}
	else
	{
		w = extract(m, position, 3);
		xy_value = fscale(m, 2, fdiv(m, 2, fsub(m, 2, fadd(m, 2, xy(m, position), shift), xy(m, viewport_offset)),
			scale), w);
	}
	m->previous = previous;
	parts[0] = extract(m, xy_value, 0);
	parts[1] = extract(m, xy_value, 1);
	parts[2] = w;
	return construct(m, 3, parts);
}

/* the program run on the previous frame's registers, from its state before
its first instruction: its position as motion_screen has it */
static unsigned int motion_previous(struct module *m, const struct xgpu_vertex_program *program,
	const struct vertex_state *initial)
{
	struct vertex_state state = *initial;
	unsigned int clip_position = 0;
	BOOL clip_captured = FALSE;
	unsigned long index;

	m->previous = TRUE;
	for (index = 0; index < program->instruction_count; index++)
	{
		const struct xgpu_vertex_instruction *instruction = &program->instructions[index];
		unsigned int operands = mac_operands(instruction->mac) |
			(instruction->ilu != XGPU_ILU_NOP && instruction->ilu <= XGPU_ILU_LIT ? 4 : 0);
		unsigned int a = 0, b = 0, c = 0;

		if (operands & 1)
			a = vertex_operand(m, &state, &instruction->a, instruction->relative);
		if (operands & 2)
			b = vertex_operand(m, &state, &instruction->b, instruction->relative);
		if (operands & 4)
			c = vertex_operand(m, &state, &instruction->c, instruction->relative);
		if (instruction->mac != XGPU_MAC_NOP)
			state.mac = mac_operation(m, &state, instruction->mac, a, b, c);
		if (instruction->ilu != XGPU_ILU_NOP)
			state.ilu = ilu_operation(m, &state, instruction->ilu, c);
		if (instruction->captures_clip)
		{
			clip_position = state.position;
			clip_captured = TRUE;
		}
		if (instruction->mac == XGPU_MAC_ARL)
		{
			state.a0 = op1(m, OP_CONVERT_F_TO_S, m->t_int, ext1(m, GLSL_FLOOR, m->t_float,
				fadd(m, 1, extract(m, state.mac, 0), constant_float(m, (float)0.001))));
		}
		else if (instruction->mac != XGPU_MAC_NOP && instruction->mac_mask)
		{
			if (instruction->mac_temporary == XGPU_VERTEX_POSITION)
				state.position = write_masked(m, state.position, state.mac, instruction->mac_mask);
			else if (instruction->mac_temporary < XGPU_VERTEX_POSITION)
				state.r[instruction->mac_temporary] = write_masked(m, state.r[instruction->mac_temporary], state.mac,
					instruction->mac_mask);
		}
		if (instruction->ilu != XGPU_ILU_NOP && instruction->ilu_mask)
		{
			if (instruction->ilu_temporary == XGPU_VERTEX_POSITION)
				state.position = write_masked(m, state.position, state.ilu, instruction->ilu_mask);
			else if (instruction->ilu_temporary < XGPU_VERTEX_POSITION)
				state.r[instruction->ilu_temporary] = write_masked(m, state.r[instruction->ilu_temporary], state.ilu,
					instruction->ilu_mask);
		}
		if (instruction->writes_output && output_slot(instruction->output) == OUT_POSITION)
			state.position = write_masked(m, state.position, instruction->output_from_ilu ? state.ilu : state.mac,
				instruction->output_mask);
	}
	m->previous = FALSE;
	return motion_screen(m, clip_captured, clip_position, state.position);
}

BOOL xgpu_spirv_vertex_shader(const struct xgpu_vertex_program *program, unsigned long packed_attribute_mask,
	unsigned long integer_attribute_mask, const struct nv2a_vertex_lighting *lighting,
	const struct xgpu_spirv_options *options, struct xgpu_spirv_module *module)
{
	static const char *const output_names[] = { "xD0", "xD1", "xT0", "xT1", "xT2", "xT3" };
	struct module m;
	struct words locals = { 0 };
	struct vertex_state state;
	unsigned long inputs, index;
	unsigned int main_id, input_variables[XGPU_VERTEX_ATTRIBUTE_COUNT];
	unsigned int position_out, point_size_out, color_outs[6], fog_out;
	unsigned int world_normal_out = 0, world_position_out = 0, world_binormal_out = 0, world_tangent_out = 0;
	unsigned int light_ambient_out = 0, light_point_out = 0;
	unsigned int world_normal = 0, world_position = 0;
	unsigned int clip_position = 0, view = 0, screen_points = 0, centre_clip = 0, eye = 0;
	BOOL environment = lighting && lighting->lights == XGPU_ENVIRONMENT_LIGHTING;
	BOOL clip_captured = FALSE;
	unsigned int one = 0;
	int slot;
	/* reversed depth: the point and the projection's rows its depth is the
	dot products of (xgpu_vertex_program_depth_rows) */
	unsigned long depth_z = 0, depth_w = 0;
	BOOL depth_rows = options->reversed_depth && xgpu_vertex_program_depth_rows(program, &depth_z, &depth_w);
	unsigned int depth_point = 0, depth_z_row = 0, depth_w_row = 0;
	/* (display.temporal_aa: the positions this frame and the frame before,
	as the screen's conversion has them; none with views) */
	BOOL motion = options->motion && !options->multiview;
	unsigned int motion_current_out = 0, motion_previous_out = 0;
	struct vertex_state initial;

	module_begin(&m, EXECUTION_MODEL_VERTEX);
	m.motion = motion;
	inputs = vertex_inputs(program, lighting);
	declare_constants(&m);
	declare_uniforms(&m);
	declare_views(&m, options->multiview, options->foveated, FALSE);

	/* the attributes read */
	for (index = 0; index < XGPU_VERTEX_ATTRIBUTE_COUNT; index++)
	{
		char variable_name[16];
		unsigned int type;

		input_variables[index] = 0;
		if (!(inputs & (1UL << index)))
			continue;
		type = (packed_attribute_mask & (1UL << index)) ? m.t_uint : (integer_attribute_mask & (1UL << index)) ? m.t_iv4 : m.t_v4;
		snprintf(variable_name, sizeof(variable_name), "v%lu", index);
		input_variables[index] = interface_variable(&m, type, STORAGE_INPUT, (unsigned int)index, variable_name);
	}
	/* (the position invariant: the passes of a surface meet exactly, as
	GLSL's invariant gl_Position) */
	position_out = variable(&m, m.t_v4, STORAGE_OUTPUT, "gl_Position");
	decorate1(&m, position_out, DECORATION_BUILT_IN, BUILT_IN_POSITION);
	decorate(&m, position_out, DECORATION_INVARIANT, 0, NULL);
	point_size_out = variable(&m, m.t_float, STORAGE_OUTPUT, "gl_PointSize");
	decorate1(&m, point_size_out, DECORATION_BUILT_IN, BUILT_IN_POINT_SIZE);
	for (slot = 0; slot < 6; slot++)
		color_outs[slot] = interface_variable(&m, m.t_v4, STORAGE_OUTPUT, XGPU_SPIRV_LOCATION_D0 + slot, output_names[slot]);
	fog_out = interface_variable(&m, m.t_float, STORAGE_OUTPUT, XGPU_SPIRV_LOCATION_FOG, "xFog");
	if (lighting)
		world_normal_out = interface_variable(&m, m.t_v4, STORAGE_OUTPUT, XGPU_SPIRV_LOCATION_WORLD_NORMAL, "xWorldNormal");
	if (lighting_position(lighting) || options->world_position_input)
		world_position_out = interface_variable(&m, m.t_v3, STORAGE_OUTPUT, XGPU_SPIRV_LOCATION_WORLD_POSITION, "xWorldPosition");
	if (environment)
	{
		world_binormal_out = interface_variable(&m, m.t_v3, STORAGE_OUTPUT, XGPU_SPIRV_LOCATION_WORLD_BINORMAL, "xWorldBinormal");
		world_tangent_out = interface_variable(&m, m.t_v3, STORAGE_OUTPUT, XGPU_SPIRV_LOCATION_WORLD_TANGENT, "xWorldTangent");
	}
	if (lighting && lighting->split && !environment)
	{
		light_ambient_out = interface_variable(&m, m.t_v3, STORAGE_OUTPUT, XGPU_SPIRV_LOCATION_LIGHT_AMBIENT, "xLightAmbient");
		light_point_out = interface_variable(&m, m.t_v3, STORAGE_OUTPUT, XGPU_SPIRV_LOCATION_LIGHT_POINT, "xLightPoint");
	}
	if (motion)
	{
		motion_current_out = interface_variable(&m, m.t_v3, STORAGE_OUTPUT, XGPU_SPIRV_LOCATION_MOTION_CURRENT,
			"xMotionCurrent");
		motion_previous_out = interface_variable(&m, m.t_v3, STORAGE_OUTPUT, XGPU_SPIRV_LOCATION_MOTION_PREVIOUS,
			"xMotionPrevious");
	}

	main_id = function_begin(&m);
	memset(&state, 0, sizeof(state));
	state.zero = constant_splat(&m, 4, 0.0f);
	one = constant_v4(&m, 0.0f, 0.0f, 0.0f, 1.0f);
	for (index = 0; index < XGPU_VERTEX_ATTRIBUTE_COUNT; index++)
	{
		if (!input_variables[index])
			state.inputs[index] = state.zero;
		else if (packed_attribute_mask & (1UL << index))
			state.inputs[index] = unpack_normpacked3(&m, load(&m, m.t_uint, input_variables[index]));
		else if (integer_attribute_mask & (1UL << index))
			state.inputs[index] = op1(&m, OP_CONVERT_S_TO_F, m.t_v4, load(&m, m.t_iv4, input_variables[index]));
		else
			state.inputs[index] = load(&m, m.t_v4, input_variables[index]);
	}
	for (index = 0; index < 12; index++)
		state.r[index] = state.zero;
	state.position = one;
	state.outputs[OUT_POSITION] = one;
	state.outputs[OUT_D0] = state.outputs[OUT_D1] = one;
	state.outputs[OUT_T0] = state.outputs[OUT_T1] = state.outputs[OUT_T2] = state.outputs[OUT_T3] = one;
	state.outputs[OUT_FOG] = constant_splat(&m, 4, 1.0f);
	state.outputs[OUT_PTS] = splat(&m, 4, uniform(&m, U_POINT_SIZE, -1));
	state.a0 = constant_int(&m, 0);
	state.mac = state.ilu = state.zero;
	initial = state;
	/* a structure's vertices are in the world: as they are */
	if (environment)
	{
		world_position = xyz(&m, state.inputs[0]);
		world_normal = vec4_of(&m, xyz(&m, state.inputs[1]), constant_float(&m, 1.0f));
	}
	else
	{
		if (lighting)
			world_normal = state.zero;
		if (lighting_position(lighting))
			world_position = constant_splat(&m, 3, 0.0f);
		/* (display.rt_reflections: a level's surface's position, in the
		world as it is: xgpu_spirv_options' world_position_input) */
		else if (options->world_position_input)
			world_position = xyz(&m, state.inputs[0]);
	}

	for (index = 0; index < program->instruction_count; index++)
	{
		const struct xgpu_vertex_instruction *instruction = &program->instructions[index];
		unsigned int operands = mac_operands(instruction->mac) |
			(instruction->ilu != XGPU_ILU_NOP && instruction->ilu <= XGPU_ILU_LIT ? 4 : 0);
		unsigned int a = 0, b = 0, c = 0;

		/* the lighting's normal and position, before the program reuses them */
		if (lighting && !environment && index == lighting->normal_instruction &&
			lighting->normal_register < XGPU_VERTEX_POSITION)
		{
			unsigned int normal = xyz(&m, state.r[lighting->normal_register]);

			world_normal = vec4_of(&m, normal, ext1(&m, GLSL_LENGTH, m.t_float, normal));
		}
		if (lighting && lighting->lights == 2 && index == lighting->position_instruction &&
			lighting->position_register < XGPU_VERTEX_POSITION)
		{
			world_position = xyz(&m, state.r[lighting->position_register]);
		}
		else if (lighting && lighting->lights == 1 && lighting->world_found && index == lighting->world_instruction &&
			lighting->world_register < XGPU_VERTEX_POSITION)
		{
			world_position = xyz(&m, state.r[lighting->world_register]);
		}
		if (operands & 1)
			a = vertex_operand(&m, &state, &instruction->a, instruction->relative);
		if (operands & 2)
			b = vertex_operand(&m, &state, &instruction->b, instruction->relative);
		if (operands & 4)
			c = vertex_operand(&m, &state, &instruction->c, instruction->relative);
		if (depth_rows && index == depth_z)
		{
			depth_point = a;
			depth_z_row = b;
		}
		if (depth_rows && index == depth_w)
			depth_w_row = b;
		if (instruction->mac != XGPU_MAC_NOP)
			state.mac = mac_operation(&m, &state, instruction->mac, a, b, c);
		if (instruction->ilu != XGPU_ILU_NOP)
			state.ilu = ilu_operation(&m, &state, instruction->ilu, c);
		/* the screen-space conversion takes the reciprocal of the clip-space
		position's w (rcc of r12.w); keep the position it converts */
		if (instruction->captures_clip)
		{
			clip_position = state.position;
			clip_captured = TRUE;
		}

		/* results are written only after both units have read their inputs */
		if (instruction->mac == XGPU_MAC_ARL)
		{
			state.a0 = op1(&m, OP_CONVERT_F_TO_S, m.t_int, ext1(&m, GLSL_FLOOR, m.t_float,
				fadd(&m, 1, extract(&m, state.mac, 0), constant_float(&m, (float)0.001))));
		}
		else if (instruction->mac != XGPU_MAC_NOP && instruction->mac_mask)
		{
			if (instruction->mac_temporary == XGPU_VERTEX_POSITION)
				state.position = write_masked(&m, state.position, state.mac, instruction->mac_mask);
			else if (instruction->mac_temporary < XGPU_VERTEX_POSITION)
				state.r[instruction->mac_temporary] = write_masked(&m, state.r[instruction->mac_temporary], state.mac,
					instruction->mac_mask);
		}
		if (instruction->ilu != XGPU_ILU_NOP && instruction->ilu_mask)
		{
			if (instruction->ilu_temporary == XGPU_VERTEX_POSITION)
				state.position = write_masked(&m, state.position, state.ilu, instruction->ilu_mask);
			else if (instruction->ilu_temporary < XGPU_VERTEX_POSITION)
				state.r[instruction->ilu_temporary] = write_masked(&m, state.r[instruction->ilu_temporary], state.ilu,
					instruction->ilu_mask);
		}
		if (instruction->writes_output && (slot = output_slot(instruction->output)) >= 0)
		{
			unsigned int *target = slot == OUT_POSITION ? &state.position : &state.outputs[slot];

			*target = write_masked(&m, *target, instruction->output_from_ilu ? state.ilu : state.mac, instruction->output_mask);
		}
	}

	if (motion)
	{
		store(&m, motion_current_out, motion_screen(&m, clip_captured, clip_position, state.position));
		store(&m, motion_previous_out, motion_previous(&m, program, &initial));
	}
	if (options->multiview)
	{
		unsigned int correction, corrected;
		static const unsigned int xy_zw[4] = { 4, 5, 2, 3 };
		static const unsigned int zw[2] = { 2, 3 };

		view = view_number(&m, options->multiview, options->foveated);
		eye = options->foveated ? op2(&m, OP_SHIFT_RIGHT_ARITHMETIC, m.t_int, view, constant_int(&m, 1)) : view;
		if (clip_captured)
		{
			centre_clip = clip_position;
			clip_position = eye_position(&m, eye, clip_position);
		}
		/* A point projected by the game follows its screen correction even
		when the vertex program captured a clip position. */
		screen_points = compare(&m, OP_F_UNORD_NOT_EQUAL, uniform(&m, U_SCREEN_POINTS, -1), constant_float(&m, 0.0f));
		correction = uniform_index(&m, U_SCREEN_CORRECTION, eye);
		corrected = fadd(&m, 2, fmul(&m, 2, xy(&m, state.position), xy(&m, correction)),
			fscale(&m, 2, shuffle(&m, 2, correction, correction, zw), extract(&m, state.position, 3)));
		state.position = shuffle(&m, 4, state.position, corrected, xy_zw);
	}

	/* the clip position (the conversion done with c[-38] and
	c[-37] undone, pixel centres at half-integers, a w of zero behind the
	camera) */
	{
		unsigned int scale_parts[3], scale, viewport_scale, viewport_offset, screen_offset, position = 0, w;
		int component;

		viewport_scale = uniform(&m, U_VIEWPORT_SCALE, -1);
		viewport_offset = uniform(&m, U_VIEWPORT_OFFSET, -1);
		screen_offset = uniform(&m, U_SCREEN_OFFSET, -1);
		for (component = 0; component < 3; component++)
		{
			unsigned int value = extract(&m, viewport_scale, component);

			scale_parts[component] = op3(&m, OP_SELECT, m.t_float,
				compare(&m, OP_F_UNORD_NOT_EQUAL, value, constant_float(&m, 0.0f)), value, constant_float(&m, 1.0f));
		}
		scale = construct(&m, 3, scale_parts);
		if (clip_captured)
		{
			unsigned int offset_parts[3], offset, sum;

			offset_parts[0] = fadd(&m, 1, constant_float(&m, 0.5f), screen_offset);
			offset_parts[1] = constant_float(&m, 0.5f);
			offset_parts[2] = constant_float(&m, 0.0f);
			offset = construct(&m, 3, offset_parts);
			w = extract(&m, clip_position, 3);
			sum = fadd(&m, 3, fmul(&m, 3, xyz(&m, clip_position),
				xyz(&m, vertex_constant_at(&m, XGPU_VERTEX_CONSTANT_BIAS - 38))),
				fscale(&m, 3, fsub(&m, 3, fadd(&m, 3, xyz(&m, vertex_constant_at(&m, XGPU_VERTEX_CONSTANT_BIAS - 37)),
				offset), xyz(&m, viewport_offset)), w));
			position = vec4_of(&m, fdiv(&m, 3, sum, scale), w);
		}
		if (!clip_captured || options->multiview)
		{
			unsigned int shift_parts[2], ndc_parts[2], ndc, screen_position;

			shift_parts[0] = fadd(&m, 1, constant_float(&m, 0.5f), screen_offset);
			shift_parts[1] = constant_float(&m, 0.5f);
			ndc_parts[0] = fadd(&m, 2, xy(&m, state.position), construct(&m, 2, shift_parts));
			ndc_parts[1] = extract(&m, state.position, 2);
			ndc = op(&m, OP_COMPOSITE_CONSTRUCT, m.t_v3, 2, ndc_parts);
			ndc = fdiv(&m, 3, fsub(&m, 3, ndc, xyz(&m, viewport_offset)), scale);
			w = extract(&m, state.position, 3);
			screen_position = vec4_of(&m, fscale(&m, 3, ndc, w), w);
			position = clip_captured ? select_n(&m, 4, screen_points, screen_position, position) : screen_position;
		}
		/* the depth reversed (xgpu_spirv_options reversed_depth): w - z, as
		one dot product where the program's z and w are the projection's
		rows' (and the screen conversion's depth is none), which keeps every
		bit of the depth the world's coordinates would round away; the draw's
		constant depth bias toward the camera */
		if (options->reversed_depth)
		{
			unsigned int z;

			w = extract(&m, position, 3);
			z = fsub(&m, 1, w, extract(&m, position, 2));
			if (depth_rows && clip_captured && depth_point && depth_z_row && depth_w_row)
			{
				unsigned int row = fsub(&m, 4, depth_w_row, depth_z_row), rows_z, identity;

				rows_z = program->instructions[depth_z].mac == XGPU_MAC_DPH ?
					fadd(&m, 1, fdot(&m, xyz(&m, depth_point), xyz(&m, row)), extract(&m, row, 3)) :
					fdot(&m, depth_point, row);
				identity = op2(&m, OP_LOGICAL_AND, m.t_bool,
					compare(&m, OP_F_ORD_EQUAL, extract(&m, vertex_constant_at(&m, XGPU_VERTEX_CONSTANT_BIAS - 38), 2),
					scale_parts[2]),
					compare(&m, OP_F_ORD_EQUAL, extract(&m, vertex_constant_at(&m, XGPU_VERTEX_CONSTANT_BIAS - 37), 2),
					extract(&m, viewport_offset, 2)));
				/* (an eye's: its correction's w row less its z row, e, times the
				centre's clip position, with the centre's w - z for its z and w
				parts, e.w (w - z) + (e.z + e.w) z + e.x x + e.y y: e is (0, 0,
				-1, 1) but for the late latch's moves, whose terms are small; a
				point the game projected to the screen itself keeps the
				centre's depth) */
				if (options->multiview && centre_clip)
				{
					unsigned int column = op2(&m, OP_I_MUL, m.t_int, eye, constant_int(&m, 4)), e[4], eye_z;
					int part;

					for (part = 0; part < 4; part++)
					{
						unsigned int correction = uniform_index(&m, U_EYE_CORRECTION, part ?
							op2(&m, OP_I_ADD, m.t_int, column, constant_int(&m, part)) : column);

						e[part] = fsub(&m, 1, extract(&m, correction, 3), extract(&m, correction, 2));
					}
					eye_z = fadd(&m, 1, fmul(&m, 1, e[3], rows_z), fmul(&m, 1, fadd(&m, 1, e[2], e[3]),
						extract(&m, centre_clip, 2)));
					eye_z = fadd(&m, 1, eye_z, fadd(&m, 1, fmul(&m, 1, e[0], extract(&m, centre_clip, 0)),
						fmul(&m, 1, e[1], extract(&m, centre_clip, 1))));
					rows_z = op3(&m, OP_SELECT, m.t_float, screen_points, rows_z, eye_z);
				}
				z = op3(&m, OP_SELECT, m.t_float, identity, rows_z, z);
			}
			z = fadd(&m, 1, z, fmul(&m, 1, uniform(&m, U_DEPTH_BIAS, -1), w));
			position = op3(&m, OP_COMPOSITE_INSERT, m.t_v4, z, position, 2);
		}
		/* (a w of zero, or not a number: behind the camera) */
		w = extract(&m, position, 3);
		position = select_n(&m, 4, op1(&m, OP_LOGICAL_NOT, m.t_bool, compare(&m, OP_F_ORD_GREATER_THAN,
			ext1(&m, GLSL_F_ABS, m.t_float, w), constant_float(&m, 0.0f))), constant_v4(&m, 0.0f, 0.0f, 0.0f, -1.0f), position);
		/* Vulkan's rows from the top of the framebuffer, y down (xgpu_spirv.h) */
		position = op3(&m, OP_COMPOSITE_INSERT, m.t_v4, fneg(&m, 1, extract(&m, position, 1)), position, 1);
		if (options->multiview && options->foveated)
		{
			static const unsigned int zw[2] = { 2, 3 }, xy_zw[4] = { 4, 5, 2, 3 };
			unsigned int correction = uniform_index(&m, U_FOVEA_VIEW, view);
			unsigned int corrected = fadd(&m, 2, fmul(&m, 2, xy(&m, position), xy(&m, correction)),
				fscale(&m, 2, shuffle(&m, 2, correction, correction, zw), extract(&m, position, 3)));

			position = shuffle(&m, 4, position, corrected, xy_zw);
			state.outputs[OUT_PTS] = op3(&m, OP_COMPOSITE_INSERT, m.t_v4,
				fmul(&m, 1, extract(&m, state.outputs[OUT_PTS], 0), extract(&m, correction, 0)), state.outputs[OUT_PTS], 0);
		}
		store(&m, position_out, position);
	}
	store(&m, point_size_out, extract(&m, state.outputs[OUT_PTS], 0));
	/* (the colours, coordinates and fog finite: finite4) */
	store(&m, color_outs[0], saturate(&m, 4, finite4(&m, state.outputs[OUT_D0])));
	store(&m, color_outs[1], saturate(&m, 4, finite4(&m, state.outputs[OUT_D1])));
	for (slot = 0; slot < 4; slot++)
		store(&m, color_outs[2 + slot], finite4(&m, state.outputs[OUT_T0 + slot]));
	store(&m, fog_out, extract(&m, finite4(&m, state.outputs[OUT_FOG]), 0));
	if (world_normal_out)
		store(&m, world_normal_out, world_normal);
	if (light_ambient_out)
	{
		unsigned int ambient, point;

		vertex_light_split(&m, lighting, world_normal, world_position, &ambient, &point);
		store(&m, light_ambient_out, ambient);
		store(&m, light_point_out, point);
	}
	if (world_position_out)
		store(&m, world_position_out, world_position);
	if (environment)
	{
		store(&m, world_binormal_out, xyz(&m, state.inputs[2]));
		store(&m, world_tangent_out, xyz(&m, state.inputs[3]));
	}
	precise_dependencies(&m, position_out);
	if (!module_end(&m, EXECUTION_MODEL_VERTEX, main_id, &locals, module))
	{
		free(locals.words);
		return FALSE;
	}
	free(locals.words);
	return TRUE;
}

/* ---------- pixel programs */

/* the pixel shader's state: the inputs, the registers (SSA values, 0 until
first needed for the lazily made ones), the textures */
struct pixel_state
{
	const struct xgpu_pixel_program *program;
	const struct xgpu_spirv_options *options;
	struct words locals;
	unsigned int in_d0, in_d1, in_t[4], in_fog, in_world_normal, in_world_position, in_world_binormal, in_world_tangent;
	/* (display.ambient_occlusion "world": the light apart of a model lit
	for each vertex, and the occlusion at the pixel once read) */
	unsigned int in_light_ambient, in_light_point, occlusion;
	unsigned int textures[4];
	unsigned int v0, v1, t[4], dots[4], r0, r1, fog;
	unsigned int zero4;
	/* the combiner stages whose constants read as 0 (display.hdr's
	self-illuminated surface without its self-illumination:
	XGPU_HDR_SELF_ILLUMINATION), a bit each */
	unsigned int zeroed_stages;
	/* the VR mode's effects (xgpu_spirv.h): gl_FragCoord, the scene's depth
	and the sprite's thickness, and texture 0's parallax (a vec2 added to its
	coordinates; 0 none) */
	unsigned int frag_coord, effect_depth, effect_thickness, t0_offset;
	/* a z-sprite's depth (xgpu_shader.h, xgpu_pixel_program_depth_stage:
	a float, the window's depth; 0 none), and for its fade as an effect the
	prelude's unprojection of the pixel without its depth and of a depth
	unit, the eye, the scene's distance and the fade's span */
	unsigned int replaced_depth;
	unsigned int effect_base, effect_column2, effect_eye, effect_to_scene, effect_span;
	/* a model's material maps (xgpu.h's XGPU_TEXTURE_MODES_MATERIAL;
	material_read): the maps' variables (0 none), and once read the
	vertex's normal and the normal map's (unit vec3s), the occlusion,
	roughness and metalness (floats; 0 where there is no such map) */
	unsigned int normal_map, orm_map;
	unsigned int vertex_normal, shaded_normal, material_occlusion, roughness, metalness;
	/* (with the one light model: the ORM map's alpha, the MCC import's
	emissive, 0 to 1: lit_emissive) */
	unsigned int material_emissive;
	/* (the unit vector from the pixel to the eye, in the world) */
	unsigned int to_eye;
	/* display.shadows "ray_traced": the dynamic lights' shadows' mask, read
	in the lights' loop (light_shadows_declare; 0 none) */
	unsigned int light_shadows;
	/* display.rt_reflections: texture 3's reflection traced
	(rt_reflection_texel: the program's rt_reflection, and the pixel's
	world position read) */
	BOOL rt_reflection;
	/* display.rt_bounce: the bounced light found once a pixel, which the
	lights' sums add (rt_bounce_declare; 0 none) */
	unsigned int rt_bounce;
	/* the one light model's forward receiver: the pass's light table
	(lt_table_declare) and its direct light evaluated here (lt_forward_add),
	the pass's lights left out of the classic sum */
	BOOL lt_forward;
	unsigned int lt_table;
	/* the one light model's shine (lt_ggx): its sum (a vec3 variable, 0
	none), the to-eye vector and the material's alpha it is found by */
	unsigned int lt_spec, lt_spec_v, lt_spec_alpha;
};

/* display.rt_bounce (below) */
static void rt_bounce_declare(struct module *m, struct pixel_state *state);
static unsigned int rt_bounce_add(struct module *m, struct pixel_state *state, unsigned int light);
/* HALO_LT_E1 (port/linux/vulkan/xgpu_vulkan_lt.h, below) */
static int lt_e1_on(void);
static int lt_light_model(void);
static int lt_shine_view(void);
/* the light-transport pass (port/linux/vulkan/xgpu_vulkan_lt.h; HALO_LT_E1
or display.ray_traced_lighting) on for the shader being made: its options'
light_transport (xgpu_spirv_pixel_shader sets it) */
static int lt_shader_on;
static unsigned int lt_e1_guides(struct module *m, struct pixel_state *state, unsigned int shaded, BOOL specular,
	unsigned int motion);
static unsigned int lt_guides_motion(struct module *m, unsigned int current_in, unsigned int previous_in);
static unsigned int lt_e1_add(struct module *m, struct pixel_state *state, unsigned int lit, unsigned int tint,
	unsigned int p, unsigned int n);
static unsigned int lt_reflection_texel(struct module *m, struct pixel_state *state, unsigned int cube);
static BOOL lt_reflection_traced(const struct pixel_state *state);
static BOOL lt_reflection_inline(const struct pixel_state *state);
static void lt_table_declare(struct module *m, struct pixel_state *state);
static unsigned int lt_forward_skips(struct module *m, struct pixel_state *state, unsigned int light);
static unsigned int lt_forward_add(struct module *m, struct pixel_state *state, unsigned int lit, unsigned int tint,
	unsigned int p, unsigned int n, unsigned int shaded);
static unsigned int lt_forward_light(struct module *m, struct pixel_state *state, unsigned int p, unsigned int n,
	unsigned int shaded, BOOL environment);

static unsigned int pixel_input(struct module *m, struct pixel_state *state, unsigned int variable_id, unsigned int type)
{
	(void)state;
	return load(m, type, variable_id);
}

/* signed_byte: a byte's value read as a two's complement byte, over 127 */
static unsigned int signed_byte(struct module *m, unsigned int x)
{
	unsigned int b = ext1(m, GLSL_FLOOR, m->t_float, fadd(m, 1, fmul(m, 1, x, constant_float(m, 255.0f)), constant_float(m, 0.5f)));

	b = op3(m, OP_SELECT, m->t_float, compare(m, OP_F_ORD_GREATER_THAN_EQUAL, b, constant_float(m, 128.0f)),
		fsub(m, 1, b, constant_float(m, 256.0f)), b);
	return fdiv(m, 1, b, constant_float(m, 127.0f));
}

static unsigned int signed_bytes(struct module *m, unsigned int rgb)
{
	unsigned int components[3];
	int component;

	for (component = 0; component < 3; component++)
		components[component] = signed_byte(m, extract(m, rgb, component));
	return construct(m, 3, components);
}

/* the input texel of a dot product stage, mapped per PSDotMapping (a vec3) */
static unsigned int dot_input(struct module *m, struct pixel_state *state, const struct xgpu_texture_stage *stage)
{
	unsigned int rgb = xyz(m, state->t[stage->input & 3]);

	switch (stage->dot_mapping)
	{
	case XGPU_DOT_ZERO_TO_ONE:
		return rgb;
	case XGPU_DOT_MINUS_ONE_TO_ONE_D3D:
		return fdiv(m, 3, fsub(m, 3, fscale(m, 3, rgb, constant_float(m, 255.0f)), constant_splat(m, 3, 128.0f)),
			constant_splat(m, 3, 127.0f));
	case XGPU_DOT_MINUS_ONE_TO_ONE:
		return signed_bytes(m, rgb);
	default:
		return fsub(m, 3, fscale(m, 3, rgb, constant_float(m, 2.0f)), constant_splat(m, 3, 1.0f));
	}
}

/* a texture's sampled image (loaded where it is used) */
static unsigned int texture_load(struct module *m, struct pixel_state *state, int stage)
{
	unsigned char sampler = state->program->stages[stage].sampler;
	unsigned int type = type_sampled_image(m, sampler == _xgpu_sampler_3d ? DIM_3D : sampler == _xgpu_sampler_cube ? DIM_CUBE : DIM_2D,
		sampler == _xgpu_sampler_2d_layered, FALSE, TRUE);

	return load(m, type, state->textures[stage]);
}

/* a texture's first level's size (an ivec2 or ivec3) */
static unsigned int image_size(struct module *m, unsigned int image, unsigned int type)
{
	unsigned int operands[2];

	m->image_query = TRUE;
	operands[0] = image;
	operands[1] = constant_int(m, 0);
	return op(m, OP_IMAGE_QUERY_SIZE_LOD, type, 2, operands);
}

/* display.smooth_lightmaps (xgpu.h's XGPU_TEXTURE_MODES_LIGHTMAP_CUBIC): the
lightmap at uv (a vec2, the texture's own coordinates) by the uniform cubic
B-spline over the 4x4 texels about it, in four bilinear lookups: along each
axis, of t = uv size - 0.5, its texel i = floor(t) and f = t - i, the weights
w0 = (1-f)^3/6, w1 = (3f^3 - 6f^2 + 4)/6, w2 = (-3f^3 + 3f^2 + 3f + 1)/6,
w3 = f^3/6, paired: g0 = w0 + w1 at (i - 0.5 + w1/g0)/size, g1 = 1 - g0 at
(i + 1.5 + w3/g1)/size (each pair's two texels mixed by the bilinear
filter; g0 and g1 at least 1/6). The weights sum to 1 (a flat area as
bilinear draws it). Each lookup by the gradients of uv itself, so that its
level and anisotropy are bilinear's (the lookups' own coordinates jump at a
texel's edge), the sampler's LOD bias (display.temporal_aa's) applied */
static unsigned int lightmap_cubic(struct module *m, unsigned int image, unsigned int uv)
{
	unsigned int raw = op(m, OP_IMAGE, type_sampled_image(m, DIM_2D, FALSE, FALSE, FALSE), 1, &image);
	unsigned int size = op1(m, OP_CONVERT_S_TO_F, m->t_v2, image_size(m, raw, m->t_iv2));
	unsigned int one = splat(m, 2, constant_float(m, 1.0f)), sixth = constant_float(m, 1.0f / 6.0f);
	unsigned int t = fsub(m, 2, fmul(m, 2, uv, size), splat(m, 2, constant_float(m, 0.5f)));
	unsigned int i = ext1(m, GLSL_FLOOR, m->t_v2, t), f = fsub(m, 2, t, i);
	unsigned int f2 = fmul(m, 2, f, f), f3 = fmul(m, 2, f2, f), r = fsub(m, 2, one, f);
	unsigned int w0 = fscale(m, 2, fmul(m, 2, fmul(m, 2, r, r), r), sixth);
	unsigned int w1 = fscale(m, 2, fadd(m, 2, fsub(m, 2, fscale(m, 2, f3, constant_float(m, 3.0f)),
		fscale(m, 2, f2, constant_float(m, 6.0f))), splat(m, 2, constant_float(m, 4.0f))), sixth);
	unsigned int w3 = fscale(m, 2, f3, sixth);
	unsigned int g0 = fadd(m, 2, w0, w1), g1 = fsub(m, 2, one, g0);
	unsigned int p0 = fdiv(m, 2, fadd(m, 2, fsub(m, 2, i, splat(m, 2, constant_float(m, 0.5f))), fdiv(m, 2, w1, g0)), size);
	unsigned int p1 = fdiv(m, 2, fadd(m, 2, fadd(m, 2, i, splat(m, 2, constant_float(m, 1.5f))), fdiv(m, 2, w3, g1)), size);
	unsigned int dx = op1(m, OP_DPDX, m->t_v2, uv), dy = op1(m, OP_DPDY, m->t_v2, uv);
	unsigned int texels[4], operands[5], row[2];
	int corner;

	for (corner = 0; corner < 4; corner++)
	{
		unsigned int parts[2];

		parts[0] = extract(m, corner & 1 ? p1 : p0, 0);
		parts[1] = extract(m, corner & 2 ? p1 : p0, 1);
		operands[0] = image;
		operands[1] = construct(m, 2, parts);
		operands[2] = IMAGE_OPERANDS_GRAD;
		operands[3] = dx;
		operands[4] = dy;
		texels[corner] = op(m, OP_IMAGE_SAMPLE_EXPLICIT_LOD, m->t_v4, 5, operands);
	}
	for (corner = 0; corner < 2; corner++)
		row[corner] = fadd(m, 4, fscale(m, 4, texels[2 * corner], extract(m, g0, 0)),
			fscale(m, 4, texels[2 * corner + 1], extract(m, g1, 0)));
	return fadd(m, 4, fscale(m, 4, row[0], extract(m, g0, 1)), fscale(m, 4, row[1], extract(m, g1, 1)));
}

/* the stage's lookup at coordinates (a vec4); where the shader draws the
border, the texel mixed with the border color by how much of the lookup's
footprint is inside the texture along each axis the border is on (xgpu_msl.c
border_mix: with the nearest texel, all or none; filtered linearly, the half
texel either side of the edge shares it with the border) */
static unsigned int sample(struct module *m, struct pixel_state *state, int stage, unsigned int coordinates)
{
	unsigned char sampler = state->program->stages[stage].sampler;
	unsigned int border = state->options->border[stage] & (sampler == _xgpu_sampler_3d ? 7 : 3);
	unsigned int image = texture_load(m, state, stage), uv, texel;
	/* (a region of the texture: the region's coordinates, the region's
	size in texels; xgpu_msl.c's region_sample) */
	unsigned int region_uv = 0, region_size = 0;

	if (sampler == _xgpu_sampler_3d || sampler == _xgpu_sampler_cube)
		uv = xyz(m, coordinates);
	else if ((state->options->border[stage] & XGPU_BORDER_REGION) && sampler == _xgpu_sampler_2d)
	{
		static const unsigned int zw[2] = { 2, 3 };
		unsigned int region = uniform(m, U_TEXTURE_SCALE, stage), extent = xy(m, region);
		unsigned int raw = op(m, OP_IMAGE, type_sampled_image(m, DIM_2D, FALSE, FALSE, FALSE), 1, &image);
		unsigned int half;

		region_uv = xy(m, coordinates);
		region_size = fmul(m, 2, op1(m, OP_CONVERT_S_TO_F, m->t_v2, image_size(m, raw, m->t_iv2)), extent);
		half = fdiv(m, 2, splat(m, 2, constant_float(m, 0.5f)), region_size);
		uv = fadd(m, 2, shuffle(m, 2, region, region, zw), fmul(m, 2, ext3(m, GLSL_F_CLAMP, m->t_v2, region_uv, half,
			fsub(m, 2, splat(m, 2, constant_float(m, 1.0f)), half)), extent));
		border = 3;
	}
	else
		uv = fmul(m, 2, xy(m, coordinates), xy(m, uniform(m, U_TEXTURE_SCALE, stage)));
	if (sampler == _xgpu_sampler_2d_layered)
	{
		unsigned int parts[2], layer = view_number(m, state->program->multiview, state->options->foveated);

		if (state->options->foveated)
		{
			static const unsigned int zw[2] = { 2, 3 };
			unsigned int correction = uniform_index(m, U_FOVEA_SAMPLE, layer);
			unsigned int raw = op(m, OP_IMAGE, type_sampled_image(m, DIM_2D, TRUE, FALSE, FALSE), 1, &image);
			unsigned int size = op1(m, OP_CONVERT_S_TO_F, m->t_v3, image_size(m, raw, m->t_iv3));
			unsigned int inset = fadd(m, 2, fmul(m, 2, uv, xy(m, correction)),
				fdiv(m, 2, shuffle(m, 2, correction, correction, zw), xy(m, size)));
			unsigned int inside = 0;
			int axis;

			/* Outside an inset, a blur or distortion reads the whole eye's
			layer. All target rows start at the top, including the eyes. */
			for (axis = 0; axis < 2; axis++)
			{
				unsigned int value = extract(m, inset, axis);
				unsigned int within = op2(m, OP_LOGICAL_AND, m->t_bool,
					compare(m, OP_F_ORD_GREATER_THAN_EQUAL, value, constant_float(m, 0.0f)),
					compare(m, OP_F_ORD_LESS_THAN_EQUAL, value, constant_float(m, 1.0f)));

				inside = axis ? op2(m, OP_LOGICAL_AND, m->t_bool, inside, within) : within;
			}
			uv = select_n(m, 2, inside, inset, uv);
			layer = op3(m, OP_SELECT, m->t_int, inside, layer,
				op2(m, OP_BITWISE_AND, m->t_int, layer, constant_int(m, ~1)));
		}
		parts[0] = uv;
		parts[1] = op1(m, OP_CONVERT_S_TO_F, m->t_float, layer);
		texel = op2(m, OP_IMAGE_SAMPLE_IMPLICIT_LOD, m->t_v4, image, op(m, OP_COMPOSITE_CONSTRUCT, m->t_v3, 2, parts));
	}
	else if (stage == 2 && state->program->lightmap_filter && sampler == _xgpu_sampler_2d && !border &&
		!(state->options->border[stage] & XGPU_BORDER_MSDF))
	{
		/* (display.smooth_lightmaps: the lightmap pass's lightmap) */
		return lightmap_cubic(m, image, uv);
	}
	else
	{
		texel = op2(m, OP_IMAGE_SAMPLE_IMPLICIT_LOD, m->t_v4, image, uv);
	}
	/* (the high-res text's distance fields: xgpu_msl.c's msdf_coverage) */
	if ((state->options->border[stage] & XGPU_BORDER_MSDF) && sampler == _xgpu_sampler_2d)
	{
		unsigned int raw = op(m, OP_IMAGE, type_sampled_image(m, DIM_2D, FALSE, FALSE, FALSE), 1, &image);
		unsigned int size = op1(m, OP_CONVERT_S_TO_F, m->t_v2, image_size(m, raw, m->t_iv2));
		unsigned int texels = op1(m, OP_FWIDTH, m->t_v2, fmul(m, 2, uv, size));
		unsigned int across = fdiv(m, 2, splat(m, 2, constant_float(m, 1.0f)),
			ext2(m, GLSL_F_MAX, m->t_v2, texels, splat(m, 2, constant_float(m, 1e-6f))));
		unsigned int pixels = ext2(m, GLSL_F_MAX, m->t_float, fmul(m, 1, constant_float(m, XGPU_MSDF_RANGE * 0.5f),
			fadd(m, 1, extract(m, across, 0), extract(m, across, 1))), constant_float(m, 1.0f));
		unsigned int red = extract(m, texel, 0), green = extract(m, texel, 1), blue = extract(m, texel, 2);
		unsigned int distance = ext2(m, GLSL_F_MAX, m->t_float, ext2(m, GLSL_F_MIN, m->t_float, red, green),
			ext2(m, GLSL_F_MIN, m->t_float, ext2(m, GLSL_F_MAX, m->t_float, red, green), blue));
		unsigned int one = constant_float(m, 1.0f);

		return construct4(m, one, one, one, saturate(m, 1, fadd(m, 1, fmul(m, 1, fsub(m, 1, distance,
			constant_float(m, 0.5f)), pixels), constant_float(m, 0.5f))));
	}
	if (border && sampler != _xgpu_sampler_cube && sampler != _xgpu_sampler_2d_layered)
	{
		unsigned int size, inside = constant_float(m, 1.0f), linear;
		int axis;

		if (region_size)
		{
			size = region_size;
			uv = region_uv;
		}
		else
		{
			unsigned int raw = op(m, OP_IMAGE, type_sampled_image(m, sampler == _xgpu_sampler_3d ? DIM_3D : DIM_2D, FALSE,
				FALSE, FALSE), 1, &image);

			size = image_size(m, raw, sampler == _xgpu_sampler_3d ? m->t_iv3 : m->t_iv2);
			size = op1(m, OP_CONVERT_S_TO_F, sampler == _xgpu_sampler_3d ? m->t_v3 : m->t_v2, size);
		}
		linear = op2(m, OP_F_UNORD_NOT_EQUAL, m->t_bool, extract(m, uniform(m, U_TEXTURE_BORDER_LINEAR, -1), stage),
			constant_float(m, 0.0f));
		for (axis = 0; axis < 3; axis++)
		{
			unsigned int x, extent, nearest, filtered;

			if (!(border & (1u << axis)))
				continue;
			x = extract(m, uv, axis);
			extent = extract(m, size, axis);
			nearest = op3(m, OP_SELECT, m->t_float, op2(m, OP_LOGICAL_AND, m->t_bool,
				compare(m, OP_F_ORD_GREATER_THAN_EQUAL, x, constant_float(m, 0.0f)),
				compare(m, OP_F_ORD_LESS_THAN, x, constant_float(m, 1.0f))), constant_float(m, 1.0f), constant_float(m, 0.0f));
			filtered = fmul(m, 1, saturate(m, 1, fadd(m, 1, fmul(m, 1, x, extent), constant_float(m, 0.5f))),
				saturate(m, 1, fadd(m, 1, fmul(m, 1, fsub(m, 1, constant_float(m, 1.0f), x), extent), constant_float(m, 0.5f))));
			inside = fmul(m, 1, inside, op3(m, OP_SELECT, m->t_float, linear, filtered, nearest));
		}
		texel = ext3(m, GLSL_F_MIX, m->t_v4, uniform(m, U_TEXTURE_BORDER, stage), texel, splat(m, 4, inside));
	}
	return texel;
}

/* a vec4 of the parts (floats) */
static unsigned int vec4_parts(struct module *m, unsigned int x, unsigned int y, unsigned int z, unsigned int w)
{
	return construct4(m, x, y, z, w);
}

/* ---------- a model's material maps (xgpu.h's XGPU_TEXTURE_MODES_MATERIAL)

Read at texture 0's coordinates (the base map's layout, which the maps are
drawn in: docs/mods.md "Material maps"). The normal map is in tangent
space, its green toward the picture's top (OpenGL's, glTF's): its frame is
found from the screen's derivatives of the world position and of the
coordinates about the vertex's normal (Schueler's cotangent frame), so a
model needs no tangents, skinned or not. The occlusion multiplies the
ambient and distant light and the reflection, as the ambient occlusion's
does; roughness widens the dynamic lights' highlights and blurs the
reflection (its cube map's levels); metalness tints the reflection by the
base map and darkens the diffuse light by up to half (the cube maps are not
the scene's light: a metal's black diffuse would read as unlit). */
#define MATERIAL_REFLECTION_BLUR 6.0f
#define MATERIAL_HIGHLIGHT_ROUGH 0.6f
#define MATERIAL_METAL_DIFFUSE 0.5f
/* the one light model's shine on a model without a roughness map (lt_ggx:
Halo's model cube maps are a soft environment, the shaders' own "glossy"
look; ~/dev/rt2-realtime-design.md section 10), and the least a map's
roughness is taken as (a mirror's highlight of a point light is a speck) */
#define LT_MODEL_ROUGHNESS 0.35f
/* a glowing part's light (the MCC import's emissive at 1: the ORM map's
alpha), linear, which the base map's colour then multiplies */
#define LT_EMISSIVE_LIGHT 1.5f
#define LT_MODEL_ROUGHEST_SMOOTH 0.08f

static unsigned int dynamic_light(struct module *m, int member, unsigned int index_id);

static void material_read(struct module *m, struct pixel_state *state, unsigned int p, unsigned int n)
{
	unsigned int xt = pixel_input(m, state, state->in_t[0], m->t_v4), w = extract(m, xt, 3), uv;
	unsigned int type = type_sampled_image(m, DIM_2D, FALSE, FALSE, TRUE);

	w = op3(m, OP_SELECT, m->t_float, compare(m, OP_F_UNORD_NOT_EQUAL, w, constant_float(m, 0.0f)), w,
		constant_float(m, 1.0f));
	uv = fmul(m, 2, fdiv(m, 2, xy(m, xt), splat(m, 2, w)), xy(m, uniform(m, U_TEXTURE_SCALE, 0)));
	state->vertex_normal = n;
	{
		/* (the eye: the dynamic lights' grid's centre, xgpu.h) */
		unsigned int to_eye = fsub(m, 3, xyz(m, dynamic_light(m, 0, constant_int(m, 0))), p);

		state->to_eye = fscale(m, 3, to_eye, ext1(m, GLSL_INVERSE_SQRT, m->t_float,
			fmax_scalar(m, 1, fdot(m, to_eye, to_eye), (float)1.0e-12)));
	}
	if (state->normal_map)
	{
		unsigned int texel = op2(m, OP_IMAGE_SAMPLE_IMPLICIT_LOD, m->t_v4, load(m, type, state->normal_map), uv);
		/* t = (rgb * 255 - 128) / 127: 128 exactly flat (as D3D's signed mapping) */
		unsigned int t = fdiv(m, 3, fsub(m, 3, fscale(m, 3, xyz(m, texel), constant_float(m, 255.0f)),
			constant_splat(m, 3, 128.0f)), constant_splat(m, 3, 127.0f));
		unsigned int dp1 = op1(m, OP_DPDX, m->t_v3, p), dp2 = op1(m, OP_DPDY, m->t_v3, p);
		unsigned int duv1 = op1(m, OP_DPDX, m->t_v2, uv), duv2 = op1(m, OP_DPDY, m->t_v2, uv);
		unsigned int dp2perp = ext2(m, GLSL_CROSS, m->t_v3, dp2, n), dp1perp = ext2(m, GLSL_CROSS, m->t_v3, n, dp1);
		unsigned int tangent, binormal, scale, shaded;

		/* T = dp2perp * duv1.x + dp1perp * duv2.x; B the same of .y (toward v, the rows down);
		scale = inversesqrt(max(dot(T, T), dot(B, B))) */
		tangent = fadd(m, 3, fscale(m, 3, dp2perp, extract(m, duv1, 0)), fscale(m, 3, dp1perp, extract(m, duv2, 0)));
		binormal = fadd(m, 3, fscale(m, 3, dp2perp, extract(m, duv1, 1)), fscale(m, 3, dp1perp, extract(m, duv2, 1)));
		scale = ext1(m, GLSL_INVERSE_SQRT, m->t_float, fmax_scalar(m, 1, ext2(m, GLSL_F_MAX, m->t_float,
			fdot(m, tangent, tangent), fdot(m, binormal, binormal)), (float)1.0e-30));
		/* normalize(T * scale * t.x - B * scale * t.y + n * max(t.z, 0.001)): green up, the top's rows */
		shaded = fadd(m, 3, fsub(m, 3, fscale(m, 3, tangent, fmul(m, 1, scale, extract(m, t, 0))),
			fscale(m, 3, binormal, fmul(m, 1, scale, extract(m, t, 1)))),
			fscale(m, 3, n, fmax_scalar(m, 1, extract(m, t, 2), 0.001f)));
		state->shaded_normal = ext1(m, GLSL_NORMALIZE, m->t_v3, shaded);
	}
	if (state->orm_map)
	{
		unsigned int texel = op2(m, OP_IMAGE_SAMPLE_IMPLICIT_LOD, m->t_v4, load(m, type, state->orm_map), uv);

		state->material_occlusion = extract(m, texel, 0);
		state->roughness = extract(m, texel, 1);
		state->metalness = extract(m, texel, 2);
		if (lt_light_model())
			state->material_emissive = extract(m, texel, 3);
	}
}

/* the reflection's texel (texture 3's cube map) with the material: the
vertex program's direction turned by as much as the eye's reflection in
the world turns from the vertex's normal to the normal map's (none where
the map is flat: the direction as it was), blurred by the roughness */
static unsigned int material_reflection(struct module *m, struct pixel_state *state, unsigned int xt)
{
	unsigned int r = xyz(m, xt), operands[4];

	if (state->shaded_normal)
	{
		unsigned int nv = state->vertex_normal, ns = state->shaded_normal, v = state->to_eye, rv, rs;

		/* rv = 2 dot(nv, v) nv - v; rs = 2 dot(ns, v) ns - v; r += length(r) * (rs - rv) */
		rv = fsub(m, 3, fscale(m, 3, nv, fmul(m, 1, fdot(m, nv, v), constant_float(m, 2.0f))), v);
		rs = fsub(m, 3, fscale(m, 3, ns, fmul(m, 1, fdot(m, ns, v), constant_float(m, 2.0f))), v);
		r = fadd(m, 3, r, fscale(m, 3, fsub(m, 3, rs, rv), ext1(m, GLSL_LENGTH, m->t_float, r)));
	}
	operands[0] = texture_load(m, state, 3);
	operands[1] = r;
	if (!state->roughness)
		return op(m, OP_IMAGE_SAMPLE_IMPLICIT_LOD, m->t_v4, 2, operands);
	operands[2] = IMAGE_OPERANDS_BIAS;
	operands[3] = fmul(m, 1, state->roughness, constant_float(m, MATERIAL_REFLECTION_BLUR));
	return op(m, OP_IMAGE_SAMPLE_IMPLICIT_LOD, m->t_v4, 4, operands);
}

static unsigned int rt_reflection_texel(struct module *m, struct pixel_state *state, unsigned int r, unsigned int cube);

static void texture_stage(struct module *m, struct pixel_state *state, int stage)
{
	const struct xgpu_pixel_program *program = state->program;
	const struct xgpu_texture_stage *decoded = &program->stages[stage];
	unsigned long mode = decoded->mode;
	unsigned int coordinates, xt = 0;
	unsigned int one = constant_float(m, 1.0f), zero = constant_float(m, 0.0f);

	if (mode != XGPU_TEXTURE_NONE && state->in_t[stage])
		xt = pixel_input(m, state, state->in_t[stage], m->t_v4);
	switch (mode)
	{
	case XGPU_TEXTURE_PROJECT2D:
	case XGPU_TEXTURE_PROJECT3D:
	{
		/* vec4(xT.xyz / (xT.w != 0.0 ? xT.w : 1.0), 1.0) */
		unsigned int w = extract(m, xt, 3);

		w = op3(m, OP_SELECT, m->t_float, compare(m, OP_F_UNORD_NOT_EQUAL, w, zero), w, one);
		coordinates = vec4_of(m, fdiv(m, 3, xyz(m, xt), splat(m, 3, w)), one);
		if (stage == 0 && state->t0_offset)
		{
			/* (an effect's parallax: effect_prelude) */
			static const unsigned int moved[4] = { 4, 5, 2, 3 };

			coordinates = shuffle(m, 4, coordinates, fadd(m, 2, xy(m, coordinates), state->t0_offset), moved);
		}
		state->t[stage] = sample(m, state, stage, coordinates);
		if (stage == 0 && program->point_threshold && decoded->sampler == _xgpu_sampler_2d)
		{
			/* IDs must not be averaged with another segment or corrupted by
			low-alpha artwork. Zero marks the atlas's continuous meters. */
			unsigned int uv = fmul(m, 2, xy(m, coordinates), xy(m, uniform(m, U_TEXTURE_SCALE, 0)));
			unsigned int image = op(m, OP_IMAGE, type_sampled_image(m, DIM_2D, FALSE, FALSE, FALSE), 1,
				(unsigned int[]){ texture_load(m, state, 0) });
			unsigned int size, pixel, threshold, operands[4];

			size = image_size(m, image, m->t_iv2);
			pixel = op1(m, OP_CONVERT_F_TO_S, m->t_iv2, ext1(m, GLSL_FLOOR, m->t_v2,
				fmul(m, 2, uv, op1(m, OP_CONVERT_S_TO_F, m->t_v2, size))));
			{
				unsigned int zeros[2], last;

				zeros[0] = zeros[1] = constant_int(m, 0);
				last = op2(m, OP_I_SUB, m->t_iv2, size, op(m, OP_COMPOSITE_CONSTRUCT, m->t_iv2, 2,
					(unsigned int[]){ constant_int(m, 1), constant_int(m, 1) }));
				pixel = ext3(m, GLSL_S_CLAMP, m->t_iv2, pixel, op(m, OP_COMPOSITE_CONSTRUCT, m->t_iv2, 2, zeros), last);
			}
			operands[0] = image;
			operands[1] = pixel;
			operands[2] = IMAGE_OPERANDS_LOD;
			operands[3] = constant_int(m, 0);
			threshold = extract(m, op(m, OP_IMAGE_FETCH, m->t_v4, 4, operands), 0);
			{
				static const unsigned int rb[4] = { 4, 1, 5, 3 };
				unsigned int replaced = shuffle(m, 4, state->t[0], splat(m, 2, threshold), rb);

				state->t[0] = select_n(m, 4, compare(m, OP_F_ORD_GREATER_THAN, threshold, zero), replaced, state->t[0]);
			}
		}
		break;
	}
	case XGPU_TEXTURE_CUBEMAP:
		if (stage == 3 && (state->shaded_normal || state->roughness))
			state->t[stage] = material_reflection(m, state, xt);
		else
			state->t[stage] = sample(m, state, stage, xt);
		/* (display.rt_reflections: the reflection's texel traced along the
		same vector) */
		if (stage == 3 && lt_reflection_inline(state))
			state->t[stage] = rt_reflection_texel(m, state, xyz(m, xt), state->t[stage]);
		else if (stage == 3 && lt_reflection_traced(state)) /* (HALO_LT_E1: the pass's own) */
			state->t[stage] = lt_reflection_texel(m, state, state->t[stage]);
		break;
	case XGPU_TEXTURE_PASSTHRU:
		state->t[stage] = saturate(m, 4, xt);
		break;
	case XGPU_TEXTURE_CLIPPLANE:
	{
		int component;

		for (component = 0; component < 4; component++)
		{
			unsigned int value = extract(m, xt, component);

			discard_if(m, compare(m, (decoded->compare & (1 << component)) ? OP_F_ORD_GREATER_THAN_EQUAL : OP_F_ORD_LESS_THAN,
				value, zero));
		}
		state->t[stage] = state->zero4;
		break;
	}
	case XGPU_TEXTURE_BUMPENVMAP:
	case XGPU_TEXTURE_BUMPENVMAP_LUMINANCE:
	{
		int input = decoded->previous >= 0 ? decoded->previous : 0;
		unsigned int d = signed_bytes(m, xyz(m, state->t[input]));
		unsigned int dx = extract(m, d, 0), dy = extract(m, d, 1);
		unsigned int matrix = uniform(m, U_BUMP_MATRIX, stage);
		unsigned int offset[2], texel;

		offset[0] = fadd(m, 1, fmul(m, 1, extract(m, matrix, 0), dx), fmul(m, 1, extract(m, matrix, 2), dy));
		offset[1] = fadd(m, 1, fmul(m, 1, extract(m, matrix, 1), dx), fmul(m, 1, extract(m, matrix, 3), dy));
		coordinates = fadd(m, 2, xy(m, xt), construct(m, 2, offset));
		{
			unsigned int parts[3];

			parts[0] = coordinates;
			parts[1] = zero;
			parts[2] = one;
			coordinates = op(m, OP_COMPOSITE_CONSTRUCT, m->t_v4, 3, parts);
		}
		texel = sample(m, state, stage, coordinates);
		if (mode == XGPU_TEXTURE_BUMPENVMAP_LUMINANCE)
		{
			unsigned int luminance = uniform(m, U_BUMP_LUMINANCE, stage);
			unsigned int factor = saturate(m, 1, fadd(m, 1, fmul(m, 1, extract(m, luminance, 0), extract(m, state->t[input], 2)),
				extract(m, luminance, 1)));
			static const unsigned int rgb_a[4] = { 4, 5, 6, 3 };

			texel = shuffle(m, 4, texel, fscale(m, 3, xyz(m, texel), factor), rgb_a);
		}
		state->t[stage] = texel;
		break;
	}
	case XGPU_TEXTURE_DOT_PRODUCT:
	case XGPU_TEXTURE_DOT_ZW:
		state->dots[stage] = fdot(m, xyz(m, xt), dot_input(m, state, decoded));
		state->t[stage] = state->zero4;
		/* the depth a DOT_ZW stage replaces the fragment's with: the previous
		dot product over its own, in the depth buffer's units; the far plane
		for a divisor that is not positive */
		if (mode == XGPU_TEXTURE_DOT_ZW && stage == xgpu_pixel_program_depth_stage(program))
		{
			state->replaced_depth = op3(m, OP_SELECT, m->t_float,
				compare(m, OP_F_ORD_GREATER_THAN, state->dots[stage], zero),
				saturate(m, 1, fdiv(m, 1, fdiv(m, 1, state->dots[stage - 1], state->dots[stage]),
					constant_float(m, XGPU_REPLACED_DEPTH_UNITS))), one);
		}
		break;
	case XGPU_TEXTURE_DOT_ST:
		state->dots[stage] = fdot(m, xyz(m, xt), dot_input(m, state, decoded));
		coordinates = vec4_parts(m, stage > 0 ? state->dots[stage - 1] : zero, state->dots[stage], zero, one);
		state->t[stage] = sample(m, state, stage, coordinates);
		break;
	case XGPU_TEXTURE_DOT_REFLECT_DIFFUSE:
		/* the normal takes its third component from stage 3's dot product */
		state->dots[stage] = fdot(m, xyz(m, xt), dot_input(m, state, decoded));
		state->dots[3] = fdot(m, xyz(m, pixel_input(m, state, state->in_t[3], m->t_v4)), dot_input(m, state, &program->stages[3]));
		coordinates = vec4_parts(m, state->dots[1], state->dots[2], state->dots[3], one);
		state->t[stage] = sample(m, state, stage, coordinates);
		break;
	case XGPU_TEXTURE_DOT_REFLECT_SPECULAR:
	case XGPU_TEXTURE_DOT_REFLECT_SPECULAR_CONSTANT:
	{
		unsigned int n, e, r, parts[3];

		state->dots[stage] = fdot(m, xyz(m, xt), dot_input(m, state, decoded));
		parts[0] = state->dots[1];
		parts[1] = state->dots[2];
		parts[2] = state->dots[3];
		n = construct(m, 3, parts);
		if (mode == XGPU_TEXTURE_DOT_REFLECT_SPECULAR)
		{
			parts[0] = extract(m, pixel_input(m, state, state->in_t[1], m->t_v4), 3);
			parts[1] = extract(m, pixel_input(m, state, state->in_t[2], m->t_v4), 3);
			parts[2] = extract(m, pixel_input(m, state, state->in_t[3], m->t_v4), 3);
			e = construct(m, 3, parts);
		}
		else
		{
			e = xyz(m, uniform(m, U_PS_C0, 0));
		}
		/* 2.0 * n * dot(n, e) / max(dot(n, n), 1.0e-20) - e */
		r = fsub(m, 3, fdiv(m, 3, fscale(m, 3, fscale(m, 3, n, constant_float(m, 2.0f)), fdot(m, n, e)),
			splat(m, 3, fmax_scalar(m, 1, fdot(m, n, n), (float)1.0e-20))), e);
		state->t[stage] = sample(m, state, stage, vec4_of(m, r, one));
		/* (display.rt_reflections: the reflection's texel traced along the
		bump map's reflection) */
		if (stage == 3 && lt_reflection_inline(state))
			state->t[stage] = rt_reflection_texel(m, state, r, state->t[stage]);
		else if (stage == 3 && lt_reflection_traced(state)) /* (HALO_LT_E1: the pass's own) */
			state->t[stage] = lt_reflection_texel(m, state, state->t[stage]);
		break;
	}
	case XGPU_TEXTURE_DOT_STR_3D:
	case XGPU_TEXTURE_DOT_STR_CUBE:
		state->dots[stage] = fdot(m, xyz(m, xt), dot_input(m, state, decoded));
		coordinates = vec4_parts(m, state->dots[1], state->dots[2], state->dots[3], one);
		state->t[stage] = sample(m, state, stage, coordinates);
		break;
	case XGPU_TEXTURE_DEPENDENT_AR:
	{
		unsigned int texel = state->t[decoded->input & 3];

		coordinates = vec4_parts(m, extract(m, texel, 3), extract(m, texel, 0), zero, one);
		state->t[stage] = sample(m, state, stage, coordinates);
		break;
	}
	case XGPU_TEXTURE_DEPENDENT_GB:
	{
		unsigned int texel = state->t[decoded->input & 3];

		coordinates = vec4_parts(m, extract(m, texel, 1), extract(m, texel, 2), zero, one);
		state->t[stage] = sample(m, state, stage, coordinates);
		break;
	}
	default:
		state->t[stage] = state->zero4;
		break;
	}

	/* channels the application marked signed (D3DTSS_COLORSIGN: bit 0
	alpha, then red, green, blue) */
	if (decoded->color_sign && mode != XGPU_TEXTURE_NONE)
	{
		static const unsigned int channels[4] = { 3, 0, 1, 2 };
		int bit;

		for (bit = 0; bit < 4; bit++)
		{
			if (decoded->color_sign & (1 << bit))
			{
				state->t[stage] = op3(m, OP_COMPOSITE_INSERT, m->t_v4,
					signed_byte(m, extract(m, state->t[stage], channels[bit])), state->t[stage], channels[bit]);
			}
		}
	}
	if (decoded->alpha_kill && mode != XGPU_TEXTURE_NONE)
		discard_if(m, compare(m, OP_F_ORD_EQUAL, extract(m, state->t[stage], 3), zero));
}

/* ---------- pixel programs: the combiners */

/* the fog register: rgb the fog color, alpha the fog factor (made the
first time it is read) */
static unsigned int fog_register(struct module *m, struct pixel_state *state)
{
	const struct xgpu_pixel_program *program = state->program;
	unsigned int factor, fog, parameters;

	if (state->fog)
		return state->fog;
	fog = pixel_input(m, state, state->in_fog, m->t_float);
	if (!program->fog)
	{
		factor = constant_float(m, 1.0f);
	}
	else if (program->fog_mode == D3DFOG_EXP)
	{
		parameters = uniform(m, U_FOG_PARAMETERS, -1);
		factor = ext1(m, GLSL_EXP, m->t_float, fmul(m, 1, fneg(m, 1, extract(m, parameters, 2)), fog));
	}
	else if (program->fog_mode == D3DFOG_EXP2)
	{
		unsigned int density;

		parameters = uniform(m, U_FOG_PARAMETERS, -1);
		density = fmul(m, 1, extract(m, parameters, 2), fog);
		factor = ext1(m, GLSL_EXP, m->t_float, fmul(m, 1, fneg(m, 1, density), density));
	}
	else if (program->fog_mode == D3DFOG_LINEAR)
	{
		unsigned int start, end;

		parameters = uniform(m, U_FOG_PARAMETERS, -1);
		start = extract(m, parameters, 0);
		end = extract(m, parameters, 1);
		factor = fdiv(m, 1, fsub(m, 1, end, fog), fmax_scalar(m, 1, fsub(m, 1, end, start), (float)1.0e-6));
	}
	else
	{
		factor = fog;
	}
	state->fog = vec4_of(m, xyz(m, uniform(m, U_FOG_COLOR, -1)), saturate(m, 1, factor));
	return state->fog;
}

/* a register's value (a vec4) for a combiner input; stage < 0 the final
combiner's */
static unsigned int register_value(struct module *m, struct pixel_state *state, unsigned long reg, int stage,
	const struct xgpu_combiner_stage *combiner, unsigned int v1r0_sum, unsigned int ef_product)
{
	if (stage >= 0 && ((state->zeroed_stages >> stage) & 1) && (reg == XGPU_REGISTER_C0 || reg == XGPU_REGISTER_C1))
		return state->zero4;
	switch (reg)
	{
	case XGPU_REGISTER_C0: return stage < 0 ? uniform(m, U_PS_FINAL_C0, -1) : uniform(m, U_PS_C0, combiner->c0 & 7);
	case XGPU_REGISTER_C1: return stage < 0 ? uniform(m, U_PS_FINAL_C1, -1) : uniform(m, U_PS_C1, combiner->c1 & 7);
	case XGPU_REGISTER_FOG: return fog_register(m, state);
	case XGPU_REGISTER_V0: return state->v0;
	case XGPU_REGISTER_V1: return state->v1;
	case XGPU_REGISTER_T0: return state->t[0];
	case XGPU_REGISTER_T1: return state->t[1];
	case XGPU_REGISTER_T2: return state->t[2];
	case XGPU_REGISTER_T3: return state->t[3];
	case XGPU_REGISTER_R0: return state->r0;
	case XGPU_REGISTER_R1: return state->r1;
	case XGPU_REGISTER_V1R0_SUM: return stage < 0 && v1r0_sum ? v1r0_sum : state->zero4;
	case XGPU_REGISTER_EF_PRODUCT: return stage < 0 && ef_product ? ef_product : state->zero4;
	default: return state->zero4;
	}
}

/* an input's value: a vec3 (rgb) or float (alpha) */
static unsigned int input_value(struct module *m, unsigned int source, const struct xgpu_combiner_input *input, BOOL alpha_portion)
{
	if (alpha_portion)
		return extract(m, source, input->alpha ? 3 : 2);
	if (input->alpha)
		return splat(m, 3, extract(m, source, 3));
	return xyz(m, source);
}

static unsigned int combiner_input(struct module *m, struct pixel_state *state, const struct xgpu_combiner_input *input,
	BOOL alpha_portion, int stage, const struct xgpu_combiner_stage *combiner)
{
	int count = alpha_portion ? 1 : 3;
	unsigned int value = input_value(m, register_value(m, state, input->reg, stage, combiner, 0, 0), input, alpha_portion);

	switch (input->mapping)
	{
	case XGPU_MAP_UNSIGNED_IDENTITY:
		return fmax_scalar(m, count, value, 0.0f);
	case XGPU_MAP_UNSIGNED_INVERT:
		return fsub(m, count, constant_splat(m, count, 1.0f), saturate(m, count, value));
	case XGPU_MAP_EXPAND_NORMAL:
		return fsub(m, count, fscale(m, count, fmax_scalar(m, count, value, 0.0f), constant_float(m, 2.0f)),
			constant_splat(m, count, 1.0f));
	case XGPU_MAP_EXPAND_NEGATE:
		return fsub(m, count, constant_splat(m, count, 1.0f),
			fscale(m, count, fmax_scalar(m, count, value, 0.0f), constant_float(m, 2.0f)));
	case XGPU_MAP_HALFBIAS_NORMAL:
		return fsub(m, count, fmax_scalar(m, count, value, 0.0f), constant_splat(m, count, 0.5f));
	case XGPU_MAP_HALFBIAS_NEGATE:
		return fsub(m, count, constant_splat(m, count, 0.5f), fmax_scalar(m, count, value, 0.0f));
	case XGPU_MAP_SIGNED_IDENTITY:
		return value;
	default:
		return fneg(m, count, value);
	}
}

/* final combiner inputs only have the unsigned identity and invert mappings
(the identity's colour not clamped above: display.hdr's XGPU_HDR_UNCLAMPED) */
static unsigned int final_input(struct module *m, struct pixel_state *state, const struct xgpu_combiner_input *input,
	BOOL alpha_portion, unsigned int v1r0_sum, unsigned int ef_product)
{
	int count = alpha_portion ? 1 : 3;
	unsigned int value = input_value(m, register_value(m, state, input->reg, -1, NULL, v1r0_sum, ef_product), input,
		alpha_portion);

	if (input->mapping == XGPU_MAP_UNSIGNED_INVERT)
		return fsub(m, count, constant_splat(m, count, 1.0f), saturate(m, count, value));
	if (XGPU_HDR_IS_UNCLAMPED(state->program->hdr) && !alpha_portion)
		return fmax_scalar(m, count, value, 0.0f);
	return saturate(m, count, value);
}

static unsigned int output_mapping(struct module *m, int count, unsigned char mapping, unsigned int value)
{
	switch (mapping)
	{
	case XGPU_OUTPUT_BIAS: return fsub(m, count, value, constant_splat(m, count, 0.5f));
	case XGPU_OUTPUT_SHIFT_LEFT_1: return fscale(m, count, value, constant_float(m, 2.0f));
	case XGPU_OUTPUT_SHIFT_LEFT_1_BIAS:
		return fscale(m, count, fsub(m, count, value, constant_splat(m, count, 0.5f)), constant_float(m, 2.0f));
	case XGPU_OUTPUT_SHIFT_LEFT_2: return fscale(m, count, value, constant_float(m, 4.0f));
	case XGPU_OUTPUT_SHIFT_RIGHT_1: return fscale(m, count, value, constant_float(m, 0.5f));
	default: return value;
	}
}

/* a destination register written: its rgb (a vec3) or alpha (a float) */
static void register_write(struct module *m, struct pixel_state *state, unsigned long reg, BOOL alpha, unsigned int value)
{
	unsigned int *target;

	switch (reg)
	{
	case XGPU_REGISTER_V0: target = &state->v0; break;
	case XGPU_REGISTER_V1: target = &state->v1; break;
	case XGPU_REGISTER_T0: target = &state->t[0]; break;
	case XGPU_REGISTER_T1: target = &state->t[1]; break;
	case XGPU_REGISTER_T2: target = &state->t[2]; break;
	case XGPU_REGISTER_T3: target = &state->t[3]; break;
	case XGPU_REGISTER_R0: target = &state->r0; break;
	case XGPU_REGISTER_R1: target = &state->r1; break;
	default: return;
	}
	if (alpha)
	{
		*target = op3(m, OP_COMPOSITE_INSERT, m->t_v4, value, *target, 3);
	}
	else
	{
		static const unsigned int rgb[4] = { 4, 5, 6, 3 };

		*target = shuffle(m, 4, *target, value, rgb);
	}
}

static void combiner_stage(struct module *m, struct pixel_state *state, int stage)
{
	const struct xgpu_pixel_program *program = state->program;
	const struct xgpu_combiner_stage *combiner = &program->combiners[stage];
	unsigned int ab[2], cd[2], sum[2];
	int portion;

	for (portion = 0; portion < 2; portion++)
	{
		const struct xgpu_combiner_portion *part = &combiner->portions[portion];
		BOOL alpha = portion == 1;
		int count = alpha ? 1 : 3;
		/* (display.hdr's XGPU_HDR_UNCLAMPED: the colour not above, a half
		float's largest) */
		float most = !alpha && XGPU_HDR_IS_UNCLAMPED(program->hdr) ? 65504.0f : 1.0f;
		unsigned int a, b, c, d;

		a = combiner_input(m, state, &part->inputs[0], alpha, stage, combiner);
		b = combiner_input(m, state, &part->inputs[1], alpha, stage, combiner);
		c = combiner_input(m, state, &part->inputs[2], alpha, stage, combiner);
		d = combiner_input(m, state, &part->inputs[3], alpha, stage, combiner);
		ab[portion] = part->ab_dot && !alpha ? splat(m, 3, fdot(m, a, b)) : fmul(m, count, a, b);
		cd[portion] = part->cd_dot && !alpha ? splat(m, 3, fdot(m, c, d)) : fmul(m, count, c, d);
		if (part->mux)
		{
			unsigned int r0a = extract(m, state->r0, 3), condition;

			if (program->mux_msb)
			{
				condition = compare(m, OP_F_ORD_GREATER_THAN_EQUAL, r0a, constant_float(m, 0.5f));
			}
			else
			{
				unsigned int bits = op1(m, OP_CONVERT_F_TO_S, m->t_int,
					fadd(m, 1, fmul(m, 1, r0a, constant_float(m, 255.0f)), constant_float(m, 0.5f)));

				condition = op2(m, OP_I_NOT_EQUAL, m->t_bool, op2(m, OP_BITWISE_AND, m->t_int, bits, constant_int(m, 1)),
					constant_int(m, 0));
			}
			sum[portion] = select_n(m, count, condition, cd[portion], ab[portion]);
		}
		else
		{
			sum[portion] = fadd(m, count, ab[portion], cd[portion]);
		}
		ab[portion] = fclamp(m, count, output_mapping(m, count, part->output_mapping, ab[portion]), -1.0f, most);
		cd[portion] = fclamp(m, count, output_mapping(m, count, part->output_mapping, cd[portion]), -1.0f, most);
		sum[portion] = fclamp(m, count, output_mapping(m, count, part->output_mapping, sum[portion]), -1.0f, most);
	}

	/* write back only after both portions have read their inputs */
	for (portion = 0; portion < 2; portion++)
	{
		const struct xgpu_combiner_portion *part = &combiner->portions[portion];
		BOOL alpha = portion == 1;

		if (part->ab != XGPU_REGISTER_ZERO)
		{
			register_write(m, state, part->ab, alpha, ab[portion]);
			if (!alpha && part->ab_blue_to_alpha)
				register_write(m, state, part->ab, TRUE, extract(m, ab[0], 2));
		}
		if (part->cd != XGPU_REGISTER_ZERO)
		{
			register_write(m, state, part->cd, alpha, cd[portion]);
			if (!alpha && part->cd_blue_to_alpha)
				register_write(m, state, part->cd, TRUE, extract(m, cd[0], 2));
		}
		if (part->sum != XGPU_REGISTER_ZERO)
			register_write(m, state, part->sum, alpha, sum[portion]);
	}
}

/* ---------- display.ambient_occlusion "world"

The occlusion found in the world at the pixel (xgpu.h's
XGPU_AMBIENT_OCCLUSION_WORLD; xgpu_msl.c's world_ao_*, operation for
operation; port/linux/game/world_ao.c's world_ao_occlusion, its CPU
reference): the window's block (XGPU_SPIRV_BINDING_WORLD_AO) and the
structure's distance field (XGPU_SPIRV_BINDING_WORLD_SDF), both in
XGPU_SPIRV_SET_OCCLUSION, declared before the function, read in it. The
proxies' loop is a structured loop, its values in the function's
variables. */

static void world_ao_declare_scenery(struct module *m)
{
	m->world_ao_scenery = variable(m, type_sampled_image(m, DIM_2D, TRUE, FALSE, TRUE), STORAGE_UNIFORM_CONSTANT,
		"world_ao_scenery");
	decorate1(m, m->world_ao_scenery, DECORATION_DESCRIPTOR_SET, XGPU_SPIRV_SET_OCCLUSION);
	decorate1(m, m->world_ao_scenery, DECORATION_BINDING, XGPU_SPIRV_BINDING_WORLD_AO_SCENERY);
}

/* (found once a pixel: the occlusion's texture, a float a pixel) */
static void world_ao_declare_pixels(struct module *m)
{
	m->world_ao_pixels = variable(m, type_sampled_image(m, DIM_2D, FALSE, FALSE, TRUE), STORAGE_UNIFORM_CONSTANT,
		"world_ao_pixels");
	decorate1(m, m->world_ao_pixels, DECORATION_DESCRIPTOR_SET, XGPU_SPIRV_SET_OCCLUSION);
	decorate1(m, m->world_ao_pixels, DECORATION_BINDING, XGPU_SPIRV_BINDING_WORLD_AO_PIXELS);
}

/* the level's field: its atlas and its table */
static void world_sdf_declare(struct module *m)
{
	m->world_sdf = variable(m, type_sampled_image(m, DIM_3D, FALSE, FALSE, TRUE), STORAGE_UNIFORM_CONSTANT, "world_sdf");
	decorate1(m, m->world_sdf, DECORATION_DESCRIPTOR_SET, XGPU_SPIRV_SET_OCCLUSION);
	decorate1(m, m->world_sdf, DECORATION_BINDING, XGPU_SPIRV_BINDING_WORLD_SDF);
	m->world_sdf_table = variable(m, type_sampled_image(m, DIM_3D, FALSE, FALSE, TRUE), STORAGE_UNIFORM_CONSTANT,
		"world_sdf_table");
	decorate1(m, m->world_sdf_table, DECORATION_DESCRIPTOR_SET, XGPU_SPIRV_SET_OCCLUSION);
	decorate1(m, m->world_sdf_table, DECORATION_BINDING, XGPU_SPIRV_BINDING_WORLD_SDF_TABLE);
}

static void world_ao_declare(struct module *m)
{
	unsigned int members[5], block, operands[3];

	members[0] = block_array(m, 2);
	members[1] = block_array(m, 2);
	members[2] = block_array(m, 3 * XGPU_WORLD_AO_SHAPES);
	members[3] = block_array(m, 2);
	/* (the grid's cells: uvec4s, two cells each) */
	members[4] = id_new(m);
	operands[0] = members[4];
	operands[1] = m->t_uv4;
	operands[2] = constant_uint(m, XGPU_WORLD_AO_GRID_CELLS / 2);
	instruction(m, SECTION_GLOBALS, OP_TYPE_ARRAY, 3, operands);
	decorate1(m, members[4], DECORATION_ARRAY_STRIDE, 16);
	block = type_struct(m, 5, members);
	decorate(m, block, DECORATION_BLOCK, 0, NULL);
	member_decorate(m, block, 0, DECORATION_OFFSET, XGPU_WORLD_AO_BLOCK_FIELD);
	member_decorate(m, block, 1, DECORATION_OFFSET, XGPU_WORLD_AO_BLOCK_PARAMETERS);
	member_decorate(m, block, 2, DECORATION_OFFSET, XGPU_WORLD_AO_BLOCK_PROXIES);
	member_decorate(m, block, 3, DECORATION_OFFSET, XGPU_WORLD_AO_BLOCK_GRID);
	member_decorate(m, block, 4, DECORATION_OFFSET, XGPU_WORLD_AO_BLOCK_CELLS);
	name(m, block, "world_ao_block");
	m->world_ao = variable(m, block, STORAGE_UNIFORM, "world_ao");
	decorate1(m, m->world_ao, DECORATION_DESCRIPTOR_SET, XGPU_SPIRV_SET_OCCLUSION);
	decorate1(m, m->world_ao, DECORATION_BINDING, XGPU_SPIRV_BINDING_WORLD_AO);
	world_sdf_declare(m);
	world_ao_declare_scenery(m);
	m->world_ao_objects = variable(m, type_sampled_image(m, DIM_3D, FALSE, FALSE, TRUE), STORAGE_UNIFORM_CONSTANT,
		"world_ao_objects");
	decorate1(m, m->world_ao_objects, DECORATION_DESCRIPTOR_SET, XGPU_SPIRV_SET_OCCLUSION);
	decorate1(m, m->world_ao_objects, DECORATION_BINDING, XGPU_SPIRV_BINDING_WORLD_AO_OBJECTS);
}

/* a vec4 of the block (0 field, 1 parameters, 2 proxies, 3 grid; 4 the
cells, a uvec4): a constant index or an int value */
static unsigned int world_ao_vector(struct module *m, int member, unsigned int index_id)
{
	unsigned int operands[3], pointer, type = member == 4 ? m->t_uv4 : m->t_v4;

	operands[0] = m->world_ao;
	operands[1] = constant_int(m, member);
	operands[2] = index_id;
	pointer = op(m, OP_ACCESS_CHAIN, type_pointer(m, STORAGE_UNIFORM, type), 3, operands);
	return load(m, type, pointer);
}

static unsigned int fselect(struct module *m, unsigned int condition, unsigned int a, unsigned int b);
static void selection_begin(struct module *m, unsigned int condition, unsigned int *merge);
static void selection_end(struct module *m, unsigned int merge);

/* the field's distance at a point (world_sdf_sample): the voxels' place
(from the first voxel's centre, clamped to the field's edge), its cell in
the table; a cell all outside or all inside its range, else its brick's
trilinear texel in the atlas, times the range */
static unsigned int world_sdf_distance(struct module *m, struct words *locals, unsigned int field0,
	unsigned int field1, unsigned int point)
{
	unsigned int range = fdiv(m, 1, constant_float(m, (float)XGPU_WORLD_SDF_RANGE_VOXELS), extract(m, field0, 3));
	unsigned int coordinate = ext3(m, GLSL_F_CLAMP, m->t_v3, fscale(m, 3, fsub(m, 3, point, xyz(m, field0)),
		extract(m, field0, 3)), constant_splat(m, 3, 0.0f), fscale(m, 3, xyz(m, field1),
		constant_float(m, (float)XGPU_WORLD_SDF_BRICK)));
	unsigned int cell = ext2(m, GLSL_F_MIN, m->t_v3, ext1(m, GLSL_FLOOR, m->t_v3, fscale(m, 3, coordinate,
		constant_float(m, 1.0f / (float)XGPU_WORLD_SDF_BRICK))), fsub(m, 3, xyz(m, field1), constant_splat(m, 3, 1.0f)));
	unsigned int sampled = load(m, type_sampled_image(m, DIM_3D, FALSE, FALSE, TRUE), m->world_sdf_table), operands[4];
	unsigned int entry, kind, result = local(m, locals, m->t_float), body = id_new(m), merge = id_new(m);
	unsigned int at, uvw, atlas, size[3], brick;

	operands[0] = op(m, OP_IMAGE, type_sampled_image(m, DIM_3D, FALSE, FALSE, FALSE), 1, &sampled);
	operands[1] = op1(m, OP_CONVERT_F_TO_S, m->t_iv3, cell);
	operands[2] = IMAGE_OPERANDS_LOD;
	operands[3] = constant_int(m, 0);
	entry = op(m, OP_IMAGE_FETCH, m->t_v4, 4, operands);
	/* (its bytes: x, y, z of the atlas's brick and the kind) */
	entry = ext1(m, GLSL_FLOOR, m->t_v4, fadd(m, 4, fscale(m, 4, entry, constant_float(m, 255.0f)),
		constant_splat(m, 4, 0.5f)));
	kind = extract(m, entry, 3);
	store(m, result, fselect(m, compare(m, OP_F_ORD_EQUAL, kind, constant_float(m, (float)XGPU_WORLD_SDF_CELL_INSIDE)),
		fneg(m, 1, range), range));
	brick = compare(m, OP_F_ORD_EQUAL, kind, constant_float(m, (float)XGPU_WORLD_SDF_CELL_BRICK));
	selection_merge(m, merge);
	branch_conditional(m, brick, body, merge);
	label_at(m, body);
	/* (the brick's texel in the atlas: its first at its place times the span) */
	at = fadd(m, 3, fadd(m, 3, fscale(m, 3, xyz(m, entry), constant_float(m, (float)XGPU_WORLD_SDF_BRICK_SPAN)),
		fsub(m, 3, coordinate, fscale(m, 3, cell, constant_float(m, (float)XGPU_WORLD_SDF_BRICK)))),
		constant_splat(m, 3, 0.5f));
	size[0] = size[1] = constant_float(m, (float)(XGPU_WORLD_SDF_ATLAS * XGPU_WORLD_SDF_BRICK_SPAN));
	size[2] = fmul(m, 1, extract(m, field1, 3), constant_float(m, (float)XGPU_WORLD_SDF_BRICK_SPAN));
	uvw = fdiv(m, 3, at, op(m, OP_COMPOSITE_CONSTRUCT, m->t_v3, 3, size));
	atlas = load(m, type_sampled_image(m, DIM_3D, FALSE, FALSE, TRUE), m->world_sdf);
	operands[0] = atlas;
	operands[1] = uvw;
	operands[2] = IMAGE_OPERANDS_LOD;
	operands[3] = constant_float(m, 0.0f);
	store(m, result, fmul(m, 1, extract(m, op(m, OP_IMAGE_SAMPLE_EXPLICIT_LOD, m->t_v4, 4, operands), 0), range));
	branch(m, merge);
	label_at(m, merge);
	return load(m, m->t_float, result);
}

/* A_sdf(p, n): the cone of taps along the normal, each tap's distance
taken from own (0, or a static scenery placement's own draw's depth inside
solid at the pixel: world_ao_shading.c's world_ao_field_occlusion_from) */
static unsigned int world_sdf_occlusion(struct module *m, struct pixel_state *state, unsigned int p, unsigned int n,
	unsigned int own)
{
	unsigned int field0 = world_ao_vector(m, 0, constant_int(m, 0)), field1 = world_ao_vector(m, 0, constant_int(m, 1));
	unsigned int step = fmax_scalar(m, 1, fdiv(m, 1, constant_float(m, 1.0f), extract(m, field0, 3)), XGPU_WORLD_AO_STEP);
	unsigned int occlusion = constant_float(m, 0.0f);
	int tap;

	for (tap = 0; tap < XGPU_WORLD_AO_TAPS; tap++)
	{
		unsigned int h = fmul(m, 1, step, constant_float(m, (float)(1 << tap)));
		unsigned int d = fsub(m, 1, world_sdf_distance(m, &state->locals, field0, field1, fadd(m, 3, p, fscale(m, 3, n, h))), own);

		occlusion = fadd(m, 1, occlusion, fmul(m, 1, constant_float(m, 1.0f / (float)(2 << tap)),
			saturate(m, 1, fdiv(m, 1, fsub(m, 1, h, d), h))));
	}
	return fsub(m, 1, constant_float(m, 1.0f), saturate(m, 1, fmul(m, 1, constant_float(m, XGPU_WORLD_AO_K), occlusion)));
}

/* A_sdf(p, n), and where cards (a two-sided model shader's: xgpu.h's
XGPU_WORLD_AO_INPUT_CARDS) that of the most open of the sky's direction
(up) and the card's two faces, the first of them where alike; *n the
direction found (world_ao_shading.c's world_ao_cards_occlusion). Its other
two cones only for the cards' pixels */
static unsigned int world_sdf_cards_occlusion(struct module *m, struct pixel_state *state, unsigned int p,
	unsigned int *n, unsigned int own, unsigned int cards)
{
	unsigned int field = world_sdf_occlusion(m, state, p, *n, own);
	unsigned int field_variable = local(m, &state->locals, m->t_float), normal_variable = local(m, &state->locals, m->t_v3);
	unsigned int merge, up, sky, back, best, direction, better;

	store(m, field_variable, field);
	store(m, normal_variable, *n);
	selection_begin(m, cards, &merge);
	up = constant_vector(m, 3, (float[]){ 0.0f, 0.0f, 1.0f, 0.0f });
	sky = world_sdf_occlusion(m, state, p, up, own);
	back = world_sdf_occlusion(m, state, p, fneg(m, 3, *n), own);
	better = compare(m, OP_F_ORD_GREATER_THAN, field, sky);
	best = fselect(m, better, field, sky);
	direction = select_n(m, 3, better, *n, up);
	better = compare(m, OP_F_ORD_GREATER_THAN, back, best);
	store(m, field_variable, fselect(m, better, back, best));
	store(m, normal_variable, select_n(m, 3, better, fneg(m, 3, *n), direction));
	selection_end(m, merge);
	*n = load(m, m->t_v3, normal_variable);
	return load(m, m->t_float, field_variable);
}

/* the shapes' occlusion, world_proxies.c's world_proxy_occlusion
(sphere_occlusion, capsule_occlusion, box_occlusion) operation for
operation, its branches as selections of values both of whose sides are
found (the side not taken may be NaN or infinite: never used; the square
roots of the side taken have their arguments as C's) */

static unsigned int fselect(struct module *m, unsigned int condition, unsigned int a, unsigned int b)
{
	return op3(m, OP_SELECT, m->t_float, condition, a, b);
}

static unsigned int fsqrt(struct module *m, unsigned int x)
{
	return ext1(m, GLSL_SQRT, m->t_float, x);
}

static unsigned int flength(struct module *m, unsigned int v)
{
	return ext1(m, GLSL_LENGTH, m->t_float, v);
}

static unsigned int fcross(struct module *m, unsigned int a, unsigned int b)
{
	return ext2(m, GLSL_CROSS, m->t_v3, a, b);
}

static unsigned int fclamp_scalar(struct module *m, unsigned int x, float low, float high)
{
	return fclamp(m, 1, x, low, high);
}

/* sphere_occlusion: centre c, radius r; 1 inside; with the horizon's
clipping where the sphere crosses p's tangent plane */
static unsigned int world_proxy_sphere(struct module *m, unsigned int p, unsigned int n, unsigned int c,
	unsigned int radius)
{
	unsigned int d = fsub(m, 3, c, p), length_squared = fdot(m, d, d), radius_squared = fmul(m, 1, radius, radius);
	unsigned int one = constant_float(m, 1.0f), length = fsqrt(m, length_squared);
	unsigned int nl = fdiv(m, 1, fdot(m, n, d), length), h2 = fdiv(m, 1, length_squared, radius_squared);
	unsigned int k2 = fsub(m, 1, one, fmul(m, 1, fmul(m, 1, h2, nl), nl)), h2_less = fsub(m, 1, h2, one);
	unsigned int cosine, clipped, plain;

	cosine = fclamp_scalar(m, fmul(m, 1, fneg(m, 1, nl), fsqrt(m, fdiv(m, 1, h2_less,
		fmax_scalar(m, 1, fsub(m, 1, one, fmul(m, 1, nl, nl)), 1.0e-30f)))), -1.0f, 1.0f);
	clipped = fadd(m, 1, fdiv(m, 1, fsub(m, 1, fmul(m, 1, nl, ext1(m, GLSL_ACOS, m->t_float, cosine)),
		fsqrt(m, fmax_scalar(m, 1, fmul(m, 1, k2, h2_less), 0.0f))), h2),
		ext1(m, GLSL_ATAN, m->t_float, fsqrt(m, fmax_scalar(m, 1, fdiv(m, 1, k2, h2_less), 0.0f))));
	clipped = fclamp_scalar(m, fdiv(m, 1, clipped, constant_float(m, XGPU_WORLD_AO_PI)), 0.0f, 1.0f);
	plain = fselect(m, compare(m, OP_F_ORD_GREATER_THAN, nl, constant_float(m, 0.0f)), fdiv(m, 1, nl, h2),
		constant_float(m, 0.0f));
	return fselect(m, compare(m, OP_F_ORD_LESS_THAN_EQUAL, length_squared, radius_squared), one,
		fselect(m, compare(m, OP_F_ORD_GREATER_THAN, k2, constant_float(m, 0.001f)), clipped, plain));
}

/* capsule_occlusion: segment a to b, radius r: its tube as a line 2r wide
above p's tangent plane, its caps' spheres weighted, never less than the
sphere at the segment's closest point */
static unsigned int world_proxy_capsule(struct module *m, unsigned int p, unsigned int n, unsigned int v0,
	unsigned int v1, unsigned int v2)
{
	unsigned int a = xyz(m, v0), b = xyz(m, v1), radius = extract(m, v2, 0), zero = constant_float(m, 0.0f);
	unsigned int half = constant_float(m, 0.5f), two = constant_float(m, 2.0f);
	unsigned int axis = fsub(m, 3, b, a), length = flength(m, axis), u = fscale(m, 3, axis, fdiv(m, 1,
		constant_float(m, 1.0f), length));
	unsigned int q = fsub(m, 3, a, p), s0 = fneg(m, 1, fdot(m, q, u)), w = fadd(m, 3, q, fscale(m, 3, u, s0));
	unsigned int h = flength(m, w), nw = fdot(m, n, w), nu = fdot(m, n, u), t0 = fneg(m, 1, s0);
	unsigned int t1 = fsub(m, 1, length, s0), horizon = fdiv(m, 1, fneg(m, 1, nw), nu);
	unsigned int parallel = compare(m, OP_F_ORD_LESS_THAN, ext1(m, GLSL_F_ABS, m->t_float, nu),
		constant_float(m, 1.0e-6f));
	unsigned int rising = compare(m, OP_F_ORD_GREATER_THAN, nu, zero);
	unsigned int from, to, h2, f[2], line, to_a, to_b, to_a_length, to_b_length, weight_a, weight_b;
	unsigned int occlusion, closest;
	int end;

	/* (above the tangent plane only) */
	from = fselect(m, parallel, t0, fselect(m, rising, ext2(m, GLSL_F_MAX, m->t_float, t0, horizon), t0));
	to = fselect(m, parallel, fselect(m, compare(m, OP_F_ORD_LESS_THAN_EQUAL, nw, zero), t0, t1),
		fselect(m, rising, t1, ext2(m, GLSL_F_MIN, m->t_float, t1, horizon)));
	h2 = fmul(m, 1, h, h);
	for (end = 0; end < 2; end++)
	{
		unsigned int t = end ? to : from, sum = fadd(m, 1, h2, fmul(m, 1, t, t));

		f[end] = fsub(m, 1, fmul(m, 1, nw, fadd(m, 1, fdiv(m, 1, t, fmul(m, 1, fmul(m, 1, two, h2), sum)),
			fdiv(m, 1, ext1(m, GLSL_ATAN, m->t_float, fdiv(m, 1, t, h)), fmul(m, 1, fmul(m, 1, two, h2), h)))),
			fdiv(m, 1, nu, fmul(m, 1, two, sum)));
	}
	line = fselect(m, op2(m, OP_LOGICAL_AND, m->t_bool, compare(m, OP_F_ORD_GREATER_THAN, to, from),
		compare(m, OP_F_ORD_GREATER_THAN, h, constant_float(m, 1.0e-4f))),
		fmul(m, 1, fdiv(m, 1, fmul(m, 1, fmul(m, 1, two, radius), h), constant_float(m, XGPU_WORLD_AO_PI)),
		fsub(m, 1, f[1], f[0])), zero);
	/* the caps */
	to_a = fsub(m, 3, p, a);
	to_b = fsub(m, 3, p, b);
	to_a_length = flength(m, to_a);
	to_b_length = flength(m, to_b);
	weight_a = fselect(m, compare(m, OP_F_ORD_GREATER_THAN, to_a_length, zero), fmul(m, 1, half, fsub(m, 1,
		constant_float(m, 1.0f), fdiv(m, 1, fdot(m, u, to_a), to_a_length))), half);
	weight_b = fselect(m, compare(m, OP_F_ORD_GREATER_THAN, to_b_length, zero), fmul(m, 1, half, fadd(m, 1,
		constant_float(m, 1.0f), fdiv(m, 1, fdot(m, u, to_b), to_b_length))), half);
	occlusion = fadd(m, 1, fadd(m, 1, line, fmul(m, 1, weight_a, world_proxy_sphere(m, p, n, a, radius))),
		fmul(m, 1, weight_b, world_proxy_sphere(m, p, n, b, radius)));
	/* the sphere at the closest point */
	closest = world_proxy_sphere(m, p, n, fadd(m, 3, a, fscale(m, 3, u, ext3(m, GLSL_F_CLAMP, m->t_float, s0, zero,
		length))), radius);
	occlusion = fclamp_scalar(m, fselect(m, compare(m, OP_F_ORD_GREATER_THAN, occlusion, closest), occlusion, closest),
		0.0f, 1.0f);
	return fselect(m, compare(m, OP_F_ORD_LESS_THAN, length, constant_float(m, 1.0e-4f)),
		world_proxy_sphere(m, p, n, a, radius), occlusion);
}

/* a polygon's edge's part of its form factor (box_occlusion's box_edge), the
corners a and b from p, the normal in box space: atan2(|a x b|, a . b) n .
(a x b) / |a x b|, the same for any lengths of a and b (none normalized); 0
where the angle's sine is under 1e-7 (either at p, or in line) */
static unsigned int world_proxy_edge(struct module *m, unsigned int normal, unsigned int a, unsigned int b)
{
	unsigned int zero = constant_float(m, 0.0f), edge_normal = fcross(m, a, b);
	unsigned int sine_squared = fdot(m, edge_normal, edge_normal), sine = fsqrt(m, sine_squared);
	unsigned int valid = compare(m, OP_F_ORD_GREATER_THAN, sine_squared, fmul(m, 1, fmul(m, 1,
		constant_float(m, 1.0e-14f), fdot(m, a, a)), fdot(m, b, b)));

	return fselect(m, valid, fdiv(m, 1, fmul(m, 1, ext2(m, GLSL_ATAN2, m->t_float, sine, fdot(m, a, b)),
		fdot(m, normal, edge_normal)), sine), zero);
}

/* face += edge(a, b) where condition holds: a selection, so that an edge
left out costs nothing */
static void world_proxy_box_add_edge(struct module *m, unsigned int face_variable, unsigned int condition,
	unsigned int normal, unsigned int a, unsigned int b)
{
	unsigned int body = id_new(m), merge = id_new(m);

	selection_merge(m, merge);
	branch_conditional(m, condition, body, merge);
	label_at(m, body);
	store(m, face_variable, fadd(m, 1, load(m, m->t_float, face_variable), world_proxy_edge(m, normal, a, b)));
	branch(m, merge);
	label_at(m, merge);
}

/* if (condition) { (the caller's code) }: the selection begun, its body's
block next; its merge in *merge, which selection_end labels */
static void selection_begin(struct module *m, unsigned int condition, unsigned int *merge)
{
	unsigned int body = id_new(m);

	*merge = id_new(m);
	selection_merge(m, *merge);
	branch_conditional(m, condition, body, *merge);
	label_at(m, body);
}

static void selection_end(struct module *m, unsigned int merge)
{
	branch(m, merge);
	label_at(m, merge);
}

/* a box at p, as box_occlusion and world_proxy_box_fade both take it: its
half extents (|v1.xyz|, |v2.xyz|, v2.w), p's offset from its centre, and p
and n along its unit axes (v1.xyz and v2.xyz made unit, and their cross);
empty where an extent is not positive. Found once for both. */
struct box_frame
{
	unsigned int extents[3], offset, at[3], local_normal[3], empty;
};

static void world_proxy_box_frame(struct module *m, unsigned int p, unsigned int n, unsigned int v0, unsigned int v1,
	unsigned int v2, struct box_frame *frame)
{
	unsigned int zero = constant_float(m, 0.0f), one = constant_float(m, 1.0f), axes[3];
	int axis;

	frame->extents[0] = flength(m, xyz(m, v1));
	frame->extents[1] = flength(m, xyz(m, v2));
	frame->extents[2] = extract(m, v2, 3);
	axes[0] = fscale(m, 3, xyz(m, v1), fdiv(m, 1, one, frame->extents[0]));
	axes[1] = fscale(m, 3, xyz(m, v2), fdiv(m, 1, one, frame->extents[1]));
	axes[2] = fcross(m, axes[0], axes[1]);
	frame->offset = fsub(m, 3, p, xyz(m, v0));
	frame->empty = constant_bool(m, FALSE);
	for (axis = 0; axis < 3; axis++)
	{
		frame->at[axis] = fdot(m, frame->offset, axes[axis]);
		frame->local_normal[axis] = fdot(m, n, axes[axis]);
		frame->empty = op2(m, OP_LOGICAL_OR, m->t_bool, frame->empty, compare(m, OP_F_ORD_LESS_THAN_EQUAL,
			frame->extents[axis], zero));
	}
}

/* the side of the box along an axis that p is beyond, if any: +1 past its
positive face, else -1 (p is outside of at most one face of each pair, the
one on its side; within the pair, neither) */
static unsigned int world_proxy_box_side(struct module *m, unsigned int at)
{
	return fselect(m, compare(m, OP_F_ORD_GREATER_THAN, at, constant_float(m, 0.0f)), constant_float(m, 1.0f),
		constant_float(m, -1.0f));
}

/* box_occlusion: centre, half axes x and y (v1.xyz, v2.xyz), half extent
along z = x cross y (v2.w): 1 inside; else the faces p is outside of, each
clipped by p's tangent plane, and their form factors, the faces taken the
same way round as seen from outside (their corners' order times the side)
so that an edge two of them share cancels and is left out: the outline's
edges alone. The clipping is Sutherland and Hodgman's taken edge by edge:
an edge with both corners above the plane is itself, one leaving it ends
where it crosses, one entering begins there, and the crossing out joins the
crossing in: the same edges as the clipped polygon's, so the same sum.
Far from it (XGPU_WORLD_AO_FAR half diagonals or more), each front face as
a patch at its centre instead. Of each pair of faces only the one on p's
side can face it, so each axis has one face, its side p's: the faces and
their sums in the same order as both of each pair taken in turn. */
static unsigned int world_proxy_box(struct module *m, struct pixel_state *state, const struct box_frame *frame)
{
	unsigned int zero = constant_float(m, 0.0f), one = constant_float(m, 1.0f), normal;
	unsigned int result_variable = local(m, &state->locals, m->t_float);
	unsigned int occlusion_variable = local(m, &state->locals, m->t_float);
	unsigned int face_variable = local(m, &state->locals, m->t_float), inside, far, outside_merge;
	unsigned int far_body = id_new(m), near_body = id_new(m), far_merge = id_new(m), far_occlusion;
	unsigned int const *extents = frame->extents, *at = frame->at;
	int axis, corner;

	normal = construct(m, 3, frame->local_normal);
	inside = constant_bool(m, TRUE);
	for (axis = 0; axis < 3; axis++)
		inside = op2(m, OP_LOGICAL_AND, m->t_bool, inside, compare(m, OP_F_ORD_LESS_THAN_EQUAL,
			ext1(m, GLSL_F_ABS, m->t_float, at[axis]), extents[axis]));
	store(m, result_variable, fselect(m, frame->empty, zero, fselect(m, inside, one, zero)));
	/* if (!empty && !inside) */
	selection_begin(m, op1(m, OP_LOGICAL_NOT, m->t_bool, op2(m, OP_LOGICAL_OR, m->t_bool, frame->empty, inside)),
		&outside_merge);
	far = compare(m, OP_F_ORD_GREATER_THAN_EQUAL, fdot(m, frame->offset, frame->offset), fmul(m, 1,
		constant_float(m, XGPU_WORLD_AO_FAR * XGPU_WORLD_AO_FAR), fadd(m, 1, fadd(m, 1, fmul(m, 1, extents[0], extents[0]),
		fmul(m, 1, extents[1], extents[1])), fmul(m, 1, extents[2], extents[2]))));
	selection_merge(m, far_merge);
	branch_conditional(m, far, far_body, near_body);
	/* far: 4 e_u e_v out (n . r) / (pi r^4) for each front face, r from p to
	the face's centre (n . r > 0) */
	label_at(m, far_body);
	far_occlusion = zero;
	for (axis = 0; axis < 3; axis++)
	{
		int u = (axis + 1) % 3, v = (axis + 2) % 3;
		unsigned int side = world_proxy_box_side(m, at[axis]);
		unsigned int out = fsub(m, 1, fmul(m, 1, side, at[axis]), extents[axis]);
		unsigned int components[3], r, r_squared, facing_patch, term;

		components[axis] = fsub(m, 1, fmul(m, 1, side, extents[axis]), at[axis]);
		components[u] = fneg(m, 1, at[u]);
		components[v] = fneg(m, 1, at[v]);
		r = construct(m, 3, components);
		r_squared = fdot(m, r, r);
		facing_patch = fdot(m, normal, r);
		term = fdiv(m, 1, fmul(m, 1, fmul(m, 1, fmul(m, 1, fmul(m, 1, constant_float(m, 4.0f), extents[u]), extents[v]),
			out), facing_patch), fmul(m, 1, fmul(m, 1, constant_float(m, XGPU_WORLD_AO_PI), r_squared), r_squared));
		far_occlusion = fselect(m, op2(m, OP_LOGICAL_AND, m->t_bool, compare(m, OP_F_ORD_GREATER_THAN, out, zero),
			compare(m, OP_F_ORD_GREATER_THAN, facing_patch, zero)), fadd(m, 1, far_occlusion, term), far_occlusion);
	}
	store(m, result_variable, fclamp_scalar(m, far_occlusion, 0.0f, 1.0f));
	branch(m, far_merge);
	/* near: the exact faces */
	label_at(m, near_body);
	store(m, occlusion_variable, zero);
	for (axis = 0; axis < 3; axis++)
	{
		int u = (axis + 1) % 3, v = (axis + 2) % 3;
		unsigned int side = world_proxy_box_side(m, at[axis]);
		unsigned int corners[4], heights[4], exit_point = 0, entry_point = 0, crossed = 0, face_merge;

		/* if (side * at[axis] > extents[axis]) { ... } (the face p is
		outside of, if any) */
		selection_begin(m, compare(m, OP_F_ORD_GREATER_THAN, fmul(m, 1, side, at[axis]), extents[axis]), &face_merge);
		store(m, face_variable, zero);
		for (corner = 0; corner < 4; corner++)
		{
			float su = (corner == 1 || corner == 2) ? 1.0f : -1.0f, sv = corner >= 2 ? 1.0f : -1.0f;
			unsigned int components[3];

			components[axis] = fsub(m, 1, fmul(m, 1, side, extents[axis]), at[axis]);
			components[u] = fsub(m, 1, fmul(m, 1, constant_float(m, su), extents[u]), at[u]);
			components[v] = fsub(m, 1, fmul(m, 1, constant_float(m, sv), extents[v]), at[v]);
			corners[corner] = construct(m, 3, components);
			heights[corner] = fdot(m, normal, corners[corner]);
		}
		for (corner = 0; corner < 4; corner++)
		{
			int next = (corner + 1) % 4;
			/* (the face across this edge: edges 0 and 2 lie on v's faces, 1
			and 3 on u's, at -, +, +, -) */
			int across = corner == 0 || corner == 2 ? v : u;
			float across_side = corner == 0 || corner == 3 ? -1.0f : 1.0f;
			unsigned int above = compare(m, OP_F_ORD_GREATER_THAN_EQUAL, heights[corner], zero);
			unsigned int next_above = compare(m, OP_F_ORD_GREATER_THAN_EQUAL, heights[next], zero);
			unsigned int t = fdiv(m, 1, heights[corner], fsub(m, 1, heights[corner], heights[next]));
			unsigned int crossing = fadd(m, 3, corners[corner], fscale(m, 3, fsub(m, 3, corners[next], corners[corner]),
				t));
			unsigned int leaves = op2(m, OP_LOGICAL_AND, m->t_bool, above, op1(m, OP_LOGICAL_NOT, m->t_bool,
				next_above));
			unsigned int enters = op2(m, OP_LOGICAL_AND, m->t_bool, op1(m, OP_LOGICAL_NOT, m->t_bool, above),
				next_above);
			unsigned int outline = compare(m, OP_F_ORD_LESS_THAN_EQUAL, fmul(m, 1, constant_float(m, across_side),
				at[across]), extents[across]);

			world_proxy_box_add_edge(m, face_variable, op2(m, OP_LOGICAL_AND, m->t_bool,
				op2(m, OP_LOGICAL_OR, m->t_bool, above, next_above), outline), normal,
				select_n(m, 3, above, corners[corner], crossing), select_n(m, 3, next_above, corners[next], crossing));
			exit_point = exit_point ? select_n(m, 3, leaves, crossing, exit_point) : select_n(m, 3, leaves, crossing,
				constant_splat(m, 3, 0.0f));
			entry_point = entry_point ? select_n(m, 3, enters, crossing, entry_point) : select_n(m, 3, enters,
				crossing, constant_splat(m, 3, 0.0f));
			crossed = crossed ? op2(m, OP_LOGICAL_OR, m->t_bool, crossed, leaves) : leaves;
		}
		world_proxy_box_add_edge(m, face_variable, crossed, normal, exit_point, entry_point);
		store(m, occlusion_variable, fadd(m, 1, load(m, m->t_float, occlusion_variable),
			fmul(m, 1, side, load(m, m->t_float, face_variable))));
		selection_end(m, face_merge);
	}
	store(m, result_variable, fclamp_scalar(m, fdiv(m, 1, ext1(m, GLSL_F_ABS, m->t_float, load(m, m->t_float,
		occlusion_variable)), constant_float(m, 2.0f * XGPU_WORLD_AO_PI)), 0.0f, 1.0f));
	branch(m, far_merge);
	label_at(m, far_merge);
	selection_end(m, outside_merge);
	return load(m, m->t_float, result_variable);
}

/* world_proxy_box_fade: the box's bound on its occlusion,
(510 * 4 / pi) (e_y e_z o_x + e_x e_z o_y + e_x e_y o_z) min(H, D) / D^4 (o
the point's distance outside each pair of faces, D = |o|, H the box's
height above the tangent plane), made a fade: clamp(bound - 1, 0, 1); 0
with no extent or wholly below the plane, 1 inside */
static unsigned int world_proxy_box_fade(struct module *m, const struct box_frame *frame)
{
	unsigned int zero = constant_float(m, 0.0f), one = constant_float(m, 1.0f), outside[3], distance_squared = zero;
	unsigned int height = zero, faces, distance, bound, fade;
	unsigned int const *extents = frame->extents;
	int axis;

	for (axis = 0; axis < 3; axis++)
	{
		unsigned int at = frame->at[axis], local_normal = frame->local_normal[axis];

		outside[axis] = fsub(m, 1, ext1(m, GLSL_F_ABS, m->t_float, at), extents[axis]);
		outside[axis] = fselect(m, compare(m, OP_F_ORD_GREATER_THAN, outside[axis], zero), outside[axis], zero);
		distance_squared = fadd(m, 1, distance_squared, fmul(m, 1, outside[axis], outside[axis]));
		height = fadd(m, 1, height, fsub(m, 1, fmul(m, 1, extents[axis], ext1(m, GLSL_F_ABS, m->t_float, local_normal)),
			fmul(m, 1, local_normal, at)));
	}
	faces = fadd(m, 1, fadd(m, 1, fmul(m, 1, fmul(m, 1, extents[1], extents[2]), outside[0]),
		fmul(m, 1, fmul(m, 1, extents[0], extents[2]), outside[1])), fmul(m, 1, fmul(m, 1, extents[0], extents[1]), outside[2]));
	distance = fsqrt(m, distance_squared);
	bound = fdiv(m, 1, fmul(m, 1, fmul(m, 1, constant_float(m, XGPU_WORLD_AO_LEAST * 4.0f / XGPU_WORLD_AO_PI), faces),
		fselect(m, compare(m, OP_F_ORD_LESS_THAN, height, distance), height, distance)),
		fmul(m, 1, distance_squared, distance_squared));
	fade = fclamp_scalar(m, fsub(m, 1, bound, one), 0.0f, 1.0f);
	fade = fselect(m, compare(m, OP_F_ORD_LESS_THAN_EQUAL, height, zero), zero, fade);
	fade = fselect(m, compare(m, OP_F_ORD_LESS_THAN_EQUAL, distance_squared, zero), one, fade);
	return fselect(m, frame->empty, zero, fade);
}

/* one shape of the pixel's cell, of a kind (XGPU_WORLD_AO_*): its factor
multiplied into the product where it can shade p (world_ao.c's
world_ao_proxy_factor): within its reach of its centre (a capsule's
segment's middle, a sphere's or a box's v0), not wholly below p's tangent
plane (its centre's height under the plane past the radius that holds it:
a sphere's own, a capsule's half length and radius, a box's half
diagonal), faded out over the last of its reach (a box by its bound), and
not faded out wholly. Each kind's reach (world_proxy_reach's): past it, its
occlusion is under 1/255. A sphere's 16 radii; a capsule's, half its length
and 16 radii of the sphere of its silhouette's area; a box's, its half
diagonal and sqrt(510 * 4 |A| / pi), A = (e_y e_z, e_x e_z, e_x e_y), where
its bound falls under 1/510. */
static void world_proxy_shade(struct module *m, struct pixel_state *state, int kind, unsigned int p, unsigned int n,
	unsigned int v0, unsigned int v1, unsigned int v2, unsigned int product_variable)
{
	unsigned int radius = extract(m, v2, 0), sixteen = constant_float(m, 16.0f), half = constant_float(m, 0.5f);
	unsigned int fade_variable = local(m, &state->locals, m->t_float);
	unsigned int centre, reach, bound, distance, reached, faded, occlusion = 0, reach_merge, fade_merge;
	struct box_frame frame;

	memset(&frame, 0, sizeof(frame));

	if (kind == XGPU_WORLD_AO_CAPSULE)
	{
		unsigned int length = flength(m, fsub(m, 3, xyz(m, v1), xyz(m, v0)));

		centre = fscale(m, 3, fadd(m, 3, xyz(m, v0), xyz(m, v1)), half);
		reach = fadd(m, 1, fmul(m, 1, half, length), fmul(m, 1, sixteen, fsqrt(m, fadd(m, 1, fmul(m, 1, radius, radius),
			fdiv(m, 1, fmul(m, 1, fmul(m, 1, constant_float(m, 2.0f), radius), length),
			constant_float(m, XGPU_WORLD_AO_PI))))));
		bound = fadd(m, 1, fmul(m, 1, half, flength(m, fsub(m, 3, xyz(m, v1), xyz(m, v0)))), radius);
	}
	else if (kind == XGPU_WORLD_AO_BOX)
	{
		unsigned int x = flength(m, xyz(m, v1)), y = flength(m, xyz(m, v2)), z = extract(m, v2, 3);
		unsigned int yz = fmul(m, 1, y, z), xz = fmul(m, 1, x, z), xy = fmul(m, 1, x, y);
		unsigned int areas = fsqrt(m, fadd(m, 1, fadd(m, 1, fmul(m, 1, yz, yz), fmul(m, 1, xz, xz)), fmul(m, 1, xy, xy)));

		centre = xyz(m, v0);
		reach = fadd(m, 1, fsqrt(m, fadd(m, 1, fadd(m, 1, fmul(m, 1, x, x), fmul(m, 1, y, y)), fmul(m, 1, z, z))),
			fsqrt(m, fmul(m, 1, constant_float(m, XGPU_WORLD_AO_LEAST * 4.0f / XGPU_WORLD_AO_PI), areas)));
		bound = fsqrt(m, fadd(m, 1, fdot(m, xyz(m, v1), xyz(m, v1)), op2(m, OP_DOT, m->t_float, v2, v2)));
	}
	else
	{
		centre = xyz(m, v0);
		reach = fmul(m, 1, sixteen, radius);
		bound = radius;
	}
	distance = ext1(m, GLSL_LENGTH, m->t_float, fsub(m, 3, p, centre));
	/* if (!(distance >= reach) && !(dot(n, centre - p) < -bound)) */
	reached = op2(m, OP_LOGICAL_AND, m->t_bool,
		op1(m, OP_LOGICAL_NOT, m->t_bool, compare(m, OP_F_ORD_GREATER_THAN_EQUAL, distance, reach)),
		op1(m, OP_LOGICAL_NOT, m->t_bool, compare(m, OP_F_ORD_LESS_THAN, fdot(m, n, fsub(m, 3, centre, p)),
		fneg(m, 1, bound))));
	selection_begin(m, reached, &reach_merge);
	if (kind == XGPU_WORLD_AO_BOX)
	{
		world_proxy_box_frame(m, p, n, v0, v1, v2, &frame);
		store(m, fade_variable, world_proxy_box_fade(m, &frame));
	}
	else
	{
		store(m, fade_variable, saturate(m, 1, fdiv(m, 1, fsub(m, 1, reach, distance), fmul(m, 1, reach,
			constant_float(m, XGPU_WORLD_AO_REACH_FADE)))));
	}
	/* if (!(fade <= 0.0)) product *= 1.0 - occlusion * fade; */
	faded = compare(m, OP_F_ORD_LESS_THAN_EQUAL, load(m, m->t_float, fade_variable), constant_float(m, 0.0f));
	selection_begin(m, op1(m, OP_LOGICAL_NOT, m->t_bool, faded), &fade_merge);
	if (kind == XGPU_WORLD_AO_SPHERE)
		occlusion = world_proxy_sphere(m, p, n, xyz(m, v0), radius);
	else if (kind == XGPU_WORLD_AO_CAPSULE)
		occlusion = world_proxy_capsule(m, p, n, v0, v1, v2);
	else
		occlusion = world_proxy_box(m, state, &frame);
	store(m, product_variable, fmul(m, 1, load(m, m->t_float, product_variable), fsub(m, 1, constant_float(m, 1.0f),
		fmul(m, 1, occlusion, load(m, m->t_float, fade_variable)))));
	selection_end(m, fade_merge);
	selection_end(m, reach_merge);
}

/* a vehicle's occlusion field (XGPU_WORLD_AO_FIELD, its shape at first and
the next two: xgpu_stream.h's XGPU_WORLD_AO_PROXY_FIELD) multiplied into the
product where it can shade p (world_ao_shading.c's
world_ao_field_proxy_occlusion): within its reach of its grid's middle,
faded out over the last of it. p's place among its voxels g, clamped to the
grid (q); past the grid, its edge's value by (L / (L + e))^2, L q's distance
from the grid's middle, e p's past the edge (voxels); the atlas's texel
there, t and v, trilinear; sat(t + n . v) of n in the vehicle's space */
static void world_proxy_field_shade(struct module *m, struct pixel_state *state, unsigned int p, unsigned int n,
	unsigned int first, unsigned int v0, unsigned int v1, unsigned int v2, unsigned int product_variable)
{
	unsigned int b0 = world_ao_vector(m, 2, op2(m, OP_I_ADD, m->t_int, first, constant_int(m, 3)));
	unsigned int b1 = world_ao_vector(m, 2, op2(m, OP_I_ADD, m->t_int, first, constant_int(m, 4)));
	unsigned int b2 = world_ao_vector(m, 2, op2(m, OP_I_ADD, m->t_int, first, constant_int(m, 5)));
	unsigned int c0 = world_ao_vector(m, 2, op2(m, OP_I_ADD, m->t_int, first, constant_int(m, 6)));
	unsigned int c1 = world_ao_vector(m, 2, op2(m, OP_I_ADD, m->t_int, first, constant_int(m, 7)));
	unsigned int c2 = world_ao_vector(m, 2, op2(m, OP_I_ADD, m->t_int, first, constant_int(m, 8)));
	unsigned int reach = extract(m, v2, 3), distance, reached, merge, kept_merge, fade, g, last, q, outside, from, scale;
	unsigned int local, uvw, sampled, occlusion, operands[4], small = constant_float(m, 1.0e-3f), zero, one;
	unsigned int extents, offset, beyond, distance_squared, height, faces, box_distance, bound, box_fade;

	zero = constant_float(m, 0.0f);
	one = constant_float(m, 1.0f);
	distance = flength(m, fsub(m, 3, p, xyz(m, v0)));
	/* if (!(distance >= reach)) */
	reached = op1(m, OP_LOGICAL_NOT, m->t_bool, compare(m, OP_F_ORD_GREATER_THAN_EQUAL, distance, reach));
	selection_begin(m, reached, &merge);
	g = fadd(m, 3, op(m, OP_COMPOSITE_CONSTRUCT, m->t_v3, 3, (unsigned int[]){ fdot(m, xyz(m, v1), p),
		fdot(m, xyz(m, b0), p), fdot(m, xyz(m, b1), p) }), xyz(m, v2));
	local = op(m, OP_COMPOSITE_CONSTRUCT, m->t_v3, 3, (unsigned int[]){ fdot(m, xyz(m, v1), n), fdot(m, xyz(m, b0), n),
		fdot(m, xyz(m, b1), n) });
	local = fscale(m, 3, local, ext1(m, GLSL_INVERSE_SQRT, m->t_float, fmax_scalar(m, 1, fdot(m, local, local), 1.0e-24f)));
	/* the geometry's box's bound (world_proxy_box_fade's, in its voxels):
	its occlusion is under it */
	extents = xyz(m, c2);
	offset = fsub(m, 3, g, fscale(m, 3, xyz(m, b2), constant_float(m, 0.5f)));
	beyond = ext2(m, GLSL_F_MAX, m->t_v3, fsub(m, 3, ext1(m, GLSL_F_ABS, m->t_v3, offset), extents),
		constant_splat(m, 3, 0.0f));
	distance_squared = fdot(m, beyond, beyond);
	height = fsub(m, 1, fdot(m, extents, ext1(m, GLSL_F_ABS, m->t_v3, local)), fdot(m, local, offset));
	faces = fdot(m, beyond, op(m, OP_COMPOSITE_CONSTRUCT, m->t_v3, 3, (unsigned int[]){
		fmul(m, 1, extract(m, extents, 1), extract(m, extents, 2)), fmul(m, 1, extract(m, extents, 0), extract(m, extents, 2)),
		fmul(m, 1, extract(m, extents, 0), extract(m, extents, 1)) }));
	box_distance = fsqrt(m, distance_squared);
	bound = fdiv(m, 1, fmul(m, 1, fmul(m, 1, constant_float(m, XGPU_WORLD_AO_LEAST * 4.0f / XGPU_WORLD_AO_PI), faces),
		fselect(m, compare(m, OP_F_ORD_LESS_THAN, height, box_distance), height, box_distance)),
		fmul(m, 1, distance_squared, distance_squared));
	box_fade = fclamp_scalar(m, fsub(m, 1, bound, one), 0.0f, 1.0f);
	box_fade = fselect(m, compare(m, OP_F_ORD_LESS_THAN_EQUAL, height, zero), zero, box_fade);
	box_fade = fselect(m, compare(m, OP_F_ORD_LESS_THAN_EQUAL, distance_squared, zero), one, box_fade);
	fade = fmul(m, 1, saturate(m, 1, fdiv(m, 1, fsub(m, 1, reach, distance), fmul(m, 1, reach,
		constant_float(m, XGPU_WORLD_AO_REACH_FADE)))), box_fade);
	/* if (!(fade <= 0.0)) */
	selection_begin(m, op1(m, OP_LOGICAL_NOT, m->t_bool, compare(m, OP_F_ORD_LESS_THAN_EQUAL, fade, zero)), &kept_merge);
	last = xyz(m, b2);
	q = ext3(m, GLSL_F_CLAMP, m->t_v3, g, constant_splat(m, 3, 0.0f), last);
	outside = flength(m, fsub(m, 3, g, q));
	from = flength(m, fsub(m, 3, q, fscale(m, 3, last, constant_float(m, 0.5f))));
	scale = fdiv(m, 1, fadd(m, 1, from, small), fadd(m, 1, fadd(m, 1, from, outside), small));
	scale = fmul(m, 1, scale, scale);
	uvw = fmul(m, 3, fadd(m, 3, q, xyz(m, c0)), xyz(m, c1));
	operands[0] = load(m, type_sampled_image(m, DIM_3D, FALSE, FALSE, TRUE), m->world_ao_objects);
	operands[1] = uvw;
	operands[2] = IMAGE_OPERANDS_LOD;
	operands[3] = constant_float(m, 0.0f);
	sampled = op(m, OP_IMAGE_SAMPLE_EXPLICIT_LOD, m->t_v4, 4, operands);
	{
		static const unsigned int vector[3] = { 1, 2, 3 };

		occlusion = saturate(m, 1, fmul(m, 1, scale, fadd(m, 1, extract(m, sampled, 0),
			fdot(m, local, shuffle(m, 3, sampled, sampled, vector)))));
	}
	store(m, product_variable, fmul(m, 1, load(m, m->t_float, product_variable), fsub(m, 1, one,
		fmul(m, 1, occlusion, fade))));
	selection_end(m, kept_merge);
	selection_end(m, merge);
	(void)state;
}

/* A_prox(p, n): the product of the shapes of the pixel's cell of the grid
(those whose reach touches it: world_ao_grid), lowest first, of the draw's
object's others (not those of its owner slot) within their reach, each
faded out over the last of it, a box by its bound (world_ao.c's
world_ao_proxies_occlusion_grid: what world_ao_proxies_occlusion finds).

With the subgroups' bitwise operations (xgpu_spirv_options subgroups), a
subgroup walks the shapes of all of its pixels' cells together, lowest
first, each pixel taking only its own cell's: each pixel's product is the
same, of the same shapes in the same order, but each shape's index, and so
its place and its kind, is the same across the subgroup. Its loads are then
of one place, and only the kinds the subgroup's shapes are of are run. The
pixels of a subgroup are near one another and mostly share their cells, and
where they differ the subgroup ran each of their shapes anyway, one lane's
at a time. */
static unsigned int world_proxies_occlusion(struct module *m, struct pixel_state *state, unsigned int p,
	unsigned int n, unsigned int owner, unsigned int limit, unsigned int analytic_from, unsigned int fields_from)
{
	unsigned int grid0 = world_ao_vector(m, 3, constant_int(m, 0)), grid1 = world_ao_vector(m, 3, constant_int(m, 1));
	unsigned int product_variable = local(m, &state->locals, m->t_float);
	unsigned int word_variable = local(m, &state->locals, m->t_int);
	unsigned int bits_variable = local(m, &state->locals, m->t_uint);
	unsigned int outer_header = id_new(m), outer_condition = id_new(m), outer_body = id_new(m);
	unsigned int outer_continue = id_new(m), outer_merge = id_new(m);
	unsigned int header = id_new(m), condition = id_new(m), body = id_new(m), next = id_new(m), merge = id_new(m);
	unsigned int operands[3], offset, at, index, cell, pair, low, word, own, bits, lowest, shape, first, v0, v1, v2;
	unsigned int kind, mine_merge = 0, owner_merge, kind_merge, analytic_merge;
	BOOL subgroups = state->options->subgroups;
	int kinds;

	/* cell = clamp(ivec3(floor(sign(offset) * log2(1.0 + abs(offset) * grid[1].w) * grid[1].xyz + size / 2)), 0,
	size - 1), offset = p - grid[0].xyz; its mask, words 2 (cell & 1) and the next of cells[cell >> 1] */
	offset = fsub(m, 3, p, xyz(m, grid0));
	at = fadd(m, 3, fmul(m, 3, fmul(m, 3, ext1(m, GLSL_F_SIGN, m->t_v3, offset),
		ext1(m, GLSL_LOG2, m->t_v3, fadd(m, 3, constant_splat(m, 3, 1.0f),
		fscale(m, 3, ext1(m, GLSL_F_ABS, m->t_v3, offset), extract(m, grid1, 3))))), xyz(m, grid1)),
		constant_vector(m, 3, (const float[]){ (float)(XGPU_WORLD_AO_GRID_X / 2), (float)(XGPU_WORLD_AO_GRID_Y / 2),
		(float)(XGPU_WORLD_AO_GRID_Z / 2) }));
	index = ext3(m, GLSL_S_CLAMP, m->t_iv3, op1(m, OP_CONVERT_F_TO_S, m->t_iv3, ext1(m, GLSL_FLOOR, m->t_v3, at)),
		op(m, OP_COMPOSITE_CONSTRUCT, m->t_iv3, 3, (unsigned int[]){ constant_int(m, 0), constant_int(m, 0), constant_int(m, 0) }),
		op(m, OP_COMPOSITE_CONSTRUCT, m->t_iv3, 3, (unsigned int[]){ constant_int(m, XGPU_WORLD_AO_GRID_X - 1),
		constant_int(m, XGPU_WORLD_AO_GRID_Y - 1), constant_int(m, XGPU_WORLD_AO_GRID_Z - 1) }));
	/* cell = (index.z * size.y + index.y) * size.x + index.x */
	cell = op2(m, OP_I_ADD, m->t_int, op2(m, OP_I_MUL, m->t_int, op2(m, OP_I_ADD, m->t_int, op2(m, OP_I_MUL, m->t_int,
		op2(m, OP_COMPOSITE_EXTRACT, m->t_int, index, 2), constant_int(m, XGPU_WORLD_AO_GRID_Y)),
		op2(m, OP_COMPOSITE_EXTRACT, m->t_int, index, 1)), constant_int(m, XGPU_WORLD_AO_GRID_X)),
		op2(m, OP_COMPOSITE_EXTRACT, m->t_int, index, 0));
	pair = world_ao_vector(m, 4, op2(m, OP_SHIFT_RIGHT_ARITHMETIC, m->t_int, cell, constant_int(m, 1)));
	low = op2(m, OP_I_MUL, m->t_int, op2(m, OP_BITWISE_AND, m->t_int, cell, constant_int(m, 1)), constant_int(m, 2));

	store(m, product_variable, constant_float(m, 1.0f));
	store(m, word_variable, constant_int(m, 0));
	branch(m, outer_header);
	label_at(m, outer_header);
	operands[0] = outer_merge;
	operands[1] = outer_continue;
	operands[2] = 0;
	op_void(m, OP_LOOP_MERGE, 3, operands);
	branch(m, outer_condition);
	label_at(m, outer_condition);
	word = load(m, m->t_int, word_variable);
	branch_conditional(m, op2(m, OP_S_LESS_THAN, m->t_bool, word, constant_int(m, 2)), outer_body, outer_merge);
	label_at(m, outer_body);
	/* this pixel's word of its cell's mask, cut to the shapes below the
	draw's limit (the dynamic objects' first: world_ao_read); walked, the
	subgroup's (all of its pixels' words or'd). allowed = clamp(limit - 32
	word, 0, 32), own &= allowed < 32 ? (1u << allowed) - 1u : ~0u */
	own = op2(m, OP_VECTOR_EXTRACT_DYNAMIC, m->t_uint, pair, op2(m, OP_I_ADD, m->t_int, low, word));
	{
		unsigned int allowed = ext3(m, GLSL_S_CLAMP, m->t_int, op2(m, OP_I_SUB, m->t_int, limit,
			op2(m, OP_I_MUL, m->t_int, word, constant_int(m, 32))), constant_int(m, 0), constant_int(m, 32));
		/* (its shift never 32, which the select leaves out anyway) */
		unsigned int shift = ext3(m, GLSL_S_CLAMP, m->t_int, allowed, constant_int(m, 0), constant_int(m, 31));
		unsigned int keep = op3(m, OP_SELECT, m->t_uint, op2(m, OP_S_LESS_THAN, m->t_bool, allowed, constant_int(m, 32)),
			op2(m, OP_I_SUB, m->t_uint, op2(m, OP_SHIFT_LEFT_LOGICAL, m->t_uint, constant_uint(m, 1), shift),
			constant_uint(m, 1)), constant_uint(m, 0xFFFFFFFFu));

		own = op2(m, OP_BITWISE_AND, m->t_uint, own, keep);
	}
	if (subgroups)
	{
		operands[0] = constant_uint(m, SCOPE_SUBGROUP);
		operands[1] = GROUP_OPERATION_REDUCE;
		operands[2] = own;
		store(m, bits_variable, op(m, OP_GROUP_NON_UNIFORM_BITWISE_OR, m->t_uint, 3, operands));
		m->group_non_uniform = TRUE;
	}
	else
	{
		store(m, bits_variable, own);
	}

	branch(m, header);
	label_at(m, header);
	operands[0] = merge;
	operands[1] = next;
	operands[2] = 0;
	op_void(m, OP_LOOP_MERGE, 3, operands);
	branch(m, condition);
	label_at(m, condition);
	bits = load(m, m->t_uint, bits_variable);
	branch_conditional(m, op2(m, OP_I_NOT_EQUAL, m->t_bool, bits, constant_uint(m, 0)), body, merge);
	label_at(m, body);
	/* shape = word * 32 + findLSB(bits); bits &= bits - 1u */
	lowest = ext1(m, GLSL_FIND_I_LSB, m->t_int, bits);
	shape = op2(m, OP_I_ADD, m->t_int, op2(m, OP_I_MUL, m->t_int, word, constant_int(m, 32)), lowest);
	store(m, bits_variable, op2(m, OP_BITWISE_AND, m->t_uint, bits, op2(m, OP_I_SUB, m->t_uint, bits, constant_uint(m, 1))));
	/* if ((own & (1u << lowest)) != 0u) (the subgroup's: this pixel's own) */
	if (subgroups)
		selection_begin(m, op2(m, OP_I_NOT_EQUAL, m->t_bool, op2(m, OP_BITWISE_AND, m->t_uint, own,
			op2(m, OP_SHIFT_LEFT_LOGICAL, m->t_uint, constant_uint(m, 1), lowest)), constant_uint(m, 0)), &mine_merge);
	first = op2(m, OP_I_MUL, m->t_int, shape, constant_int(m, 3));
	v0 = world_ao_vector(m, 2, first);
	v1 = world_ao_vector(m, 2, op2(m, OP_I_ADD, m->t_int, first, constant_int(m, 1)));
	v2 = world_ao_vector(m, 2, op2(m, OP_I_ADD, m->t_int, first, constant_int(m, 2)));
	kind = extract(m, v0, 3);
	/* if (v1.w != owner) (not the draw's object's own) */
	selection_begin(m, op1(m, OP_LOGICAL_NOT, m->t_bool, compare(m, OP_F_ORD_EQUAL, extract(m, v1, 3), owner)),
		&owner_merge);
	/* if (kind == ...) { its own reach, bound, fade and occlusion }: the
	spheres, capsules and boxes from analytic_from (below it the pass's
	rays find them: xgpu_msl.c's world_proxies_occlusion) */
	selection_begin(m, op1(m, OP_LOGICAL_NOT, m->t_bool, op2(m, OP_S_LESS_THAN, m->t_bool, shape, analytic_from)),
		&analytic_merge);
	for (kinds = 0; kinds < 3; kinds++)
	{
		static const int each[3] = { XGPU_WORLD_AO_SPHERE, XGPU_WORLD_AO_CAPSULE, XGPU_WORLD_AO_BOX };

		selection_begin(m, compare(m, OP_F_ORD_EQUAL, kind, constant_float(m, (float)each[kinds])), &kind_merge);
		world_proxy_shade(m, state, each[kinds], p, n, v0, v1, v2, product_variable);
		selection_end(m, kind_merge);
	}
	selection_end(m, analytic_merge);
	/* (and the occlusion fields from fields_from, where it is given: below
	it, with "ray_traced", the GPU's rays find their objects) */
	selection_begin(m, fields_from ? op2(m, OP_LOGICAL_AND, m->t_bool, compare(m, OP_F_ORD_EQUAL, kind,
		constant_float(m, (float)XGPU_WORLD_AO_FIELD)), op1(m, OP_LOGICAL_NOT, m->t_bool, op2(m, OP_S_LESS_THAN, m->t_bool,
		shape, fields_from))) : compare(m, OP_F_ORD_EQUAL, kind, constant_float(m, (float)XGPU_WORLD_AO_FIELD)),
		&kind_merge);
	world_proxy_field_shade(m, state, p, n, first, v0, v1, v2, product_variable);
	selection_end(m, kind_merge);
	selection_end(m, owner_merge);
	if (subgroups)
		selection_end(m, mine_merge);
	branch(m, next);
	label_at(m, next);
	branch(m, header);
	label_at(m, merge);
	branch(m, outer_continue);
	label_at(m, outer_continue);
	store(m, word_variable, op2(m, OP_I_ADD, m->t_int, load(m, m->t_int, word_variable), constant_int(m, 1)));
	branch(m, outer_header);
	label_at(m, outer_merge);
	return load(m, m->t_float, product_variable);
}

/* ---------- the distance-field scene's ray step

world_trace: port/linux/game/world_cascades.c's world_trace_ray operation
for operation (tools/test_world_trace.py runs it against it): from origin
along the unit direction to at most t_max, through the cascades (their four
vec4s: the first voxel's centre and the voxel, 0 not ready; their texture,
cascades_texture, a combined image sampler of RG8 snorm, the objects'
distance its second channel), hitting what mask (an int:
XGPU_WORLD_TRACE_*) allows, each distance only once the ray is outside it.
The structure's distance is the level's field's at the ray's point
(world_sdf_distance; field its block's two vec4s), read only where field
is given: NULL, the ray step meets the objects alone (the AO pass's). A structured loop, its values in locals; the hit (1 or 0), t
and kind (as floats) in result. A hardware backend's ray queries (#602)
would take the same arguments (docs/world-ao.md) */
/* the cascade a point is in: the finest ready, from first on, whose window
holds it with its filter's neighbours (-1 none; world_cascades.c's
world_cascades_find) */
static unsigned int world_cascade_find(struct module *m, const unsigned int cascades[XGPU_WORLD_CASCADES], unsigned int p,
	int first)
{
	unsigned int found;
	int cascade;

	found = constant_int(m, -1);
	for (cascade = XGPU_WORLD_CASCADES - 1; cascade >= first; cascade--)
	{
		unsigned int c = cascades[cascade], w = extract(m, c, 3), q, least, most, inside_window;

		q = fdiv(m, 3, fsub(m, 3, p, xyz(m, c)), splat(m, 3, w));
		least = ext2(m, GLSL_F_MIN, m->t_float, ext2(m, GLSL_F_MIN, m->t_float, extract(m, q, 0), extract(m, q, 1)),
			extract(m, q, 2));
		most = ext2(m, GLSL_F_MAX, m->t_float, ext2(m, GLSL_F_MAX, m->t_float, extract(m, q, 0), extract(m, q, 1)),
			extract(m, q, 2));
		inside_window = op2(m, OP_LOGICAL_AND, m->t_bool, op2(m, OP_LOGICAL_AND, m->t_bool,
			compare(m, OP_F_ORD_GREATER_THAN, w, constant_float(m, 0.0f)),
			compare(m, OP_F_ORD_GREATER_THAN_EQUAL, least, constant_float(m, 0.0f))),
			compare(m, OP_F_ORD_LESS_THAN_EQUAL, most, constant_float(m, (float)(XGPU_WORLD_CASCADE_SIDE - 1))));
		found = op3(m, OP_SELECT, m->t_int, inside_window, constant_int(m, cascade), found);
	}
	return found;
}

/* its distances at a point (world units: a vec4's y, the objects'; x is
none), and its voxel (world_cascades_sample) */
static unsigned int world_cascade_sample(struct module *m, unsigned int cascades_texture,
	const unsigned int cascades[XGPU_WORLD_CASCADES], unsigned int found, unsigned int p, unsigned int *voxel_out)
{
	unsigned int operands[4], chosen[4], voxel, at, place, uvw[3], sampled, range;
	int cascade, component;

	/* (its vec4, each component chosen by found) */
	for (component = 0; component < 4; component++)
	{
		chosen[component] = extract(m, cascades[0], component);
		for (cascade = 1; cascade < XGPU_WORLD_CASCADES; cascade++)
			chosen[component] = op3(m, OP_SELECT, m->t_float, op2(m, OP_I_EQUAL, m->t_bool, found,
				constant_int(m, cascade)), extract(m, cascades[cascade], component), chosen[component]);
	}
	voxel = chosen[3];
	/* at = p / voxel - 0.5; place = at - side * floor(at / side); the
	texel's coordinates (place + 0.5) / span, z's in its cascade's layers */
	at = fsub(m, 3, fdiv(m, 3, p, splat(m, 3, voxel)), constant_splat(m, 3, 0.5f));
	place = fsub(m, 3, at, fscale(m, 3, ext1(m, GLSL_FLOOR, m->t_v3, fdiv(m, 3, at,
		constant_splat(m, 3, (float)XGPU_WORLD_CASCADE_SIDE))), constant_float(m, (float)XGPU_WORLD_CASCADE_SIDE)));
	uvw[0] = fdiv(m, 1, fadd(m, 1, extract(m, place, 0), constant_float(m, 0.5f)),
		constant_float(m, (float)XGPU_WORLD_CASCADE_SPAN));
	uvw[1] = fdiv(m, 1, fadd(m, 1, extract(m, place, 1), constant_float(m, 0.5f)),
		constant_float(m, (float)XGPU_WORLD_CASCADE_SPAN));
	uvw[2] = fdiv(m, 1, fadd(m, 1, fadd(m, 1, extract(m, place, 2), constant_float(m, 0.5f)),
		fmul(m, 1, constant_float(m, (float)XGPU_WORLD_CASCADE_SPAN), op1(m, OP_CONVERT_S_TO_F, m->t_float, found))),
		constant_float(m, (float)(XGPU_WORLD_CASCADE_SPAN * XGPU_WORLD_CASCADES)));
	operands[0] = load(m, type_sampled_image(m, DIM_3D, FALSE, FALSE, TRUE), cascades_texture);
	operands[1] = construct(m, 3, uvw);
	operands[2] = IMAGE_OPERANDS_LOD;
	operands[3] = constant_float(m, 0.0f);
	range = fmul(m, 1, constant_float(m, (float)XGPU_WORLD_CASCADE_RANGE_VOXELS), voxel);
	sampled = fscale(m, 4, op(m, OP_IMAGE_SAMPLE_EXPLICIT_LOD, m->t_v4, 4, operands), range);
	*voxel_out = voxel;
	return sampled;
}

static void world_trace(struct module *m, struct words *locals, unsigned int cascades_texture,
	const unsigned int cascades[XGPU_WORLD_CASCADES], const unsigned int *field, unsigned int origin,
	unsigned int direction, unsigned int t_max, unsigned int mask, unsigned int result[3])
{
	unsigned int t_variable = local(m, locals, m->t_float), step_variable = local(m, locals, m->t_int);
	unsigned int done_variable = local(m, locals, m->t_bool), structure_variable = local(m, locals, m->t_bool);
	unsigned int objects_variable = local(m, locals, m->t_bool), hit_variable = local(m, locals, m->t_float);
	unsigned int distance_variable = local(m, locals, m->t_float), kind_variable = local(m, locals, m->t_float);
	unsigned int header = id_new(m), condition = id_new(m), body = id_new(m), next = id_new(m), merge = id_new(m);
	unsigned int operands[3], t, p, found, voxel, sampled, d[2], nearest, inside;
	unsigned int range, enabled[2], active[2], variables[2], is_hit, kind, advance, t_next, found_merge, hit_merge;
	int channel;

	store(m, t_variable, constant_float(m, 0.0f));
	store(m, step_variable, constant_int(m, 0));
	store(m, done_variable, constant_bool(m, FALSE));
	store(m, structure_variable, constant_bool(m, FALSE));
	store(m, objects_variable, constant_bool(m, FALSE));
	store(m, hit_variable, constant_float(m, 0.0f));
	store(m, distance_variable, t_max);
	store(m, kind_variable, constant_float(m, 0.0f));
	branch(m, header);
	label_at(m, header);
	operands[0] = merge;
	operands[1] = next;
	operands[2] = 0;
	op_void(m, OP_LOOP_MERGE, 3, operands);
	branch(m, condition);
	label_at(m, condition);
	branch_conditional(m, op2(m, OP_LOGICAL_AND, m->t_bool, op2(m, OP_S_LESS_THAN, m->t_bool,
		load(m, m->t_int, step_variable), constant_int(m, XGPU_WORLD_TRACE_STEPS)),
		op1(m, OP_LOGICAL_NOT, m->t_bool, load(m, m->t_bool, done_variable))), body, merge);
	label_at(m, body);
	t = load(m, m->t_float, t_variable);
	p = fadd(m, 3, origin, fscale(m, 3, direction, t));
	/* the cascade: the finest ready whose window holds p with its filter's
	neighbours (-1 none); q = (p - c.xyz) / c.w within 0 and the side less
	one */
	found = world_cascade_find(m, cascades, p, 0);
	store(m, done_variable, op2(m, OP_S_LESS_THAN, m->t_bool, found, constant_int(m, 0)));
	selection_begin(m, op1(m, OP_LOGICAL_NOT, m->t_bool, op2(m, OP_S_LESS_THAN, m->t_bool, found, constant_int(m, 0))),
		&found_merge);
	sampled = world_cascade_sample(m, cascades_texture, cascades, found, p, &voxel);
	range = fmul(m, 1, constant_float(m, (float)XGPU_WORLD_CASCADE_RANGE_VOXELS), voxel);
	/* (the structure's from the level's field where the ray is, the
	objects' from the cascade) */
	d[0] = field ? world_sdf_distance(m, locals, field[0], field[1], p) : 0;
	d[1] = extract(m, sampled, 1);
	/* each channel the mask allows, once the ray is outside it; inside one
	yet, how far it is to its way out */
	nearest = inside = range;
	variables[0] = structure_variable;
	variables[1] = objects_variable;
	for (channel = field ? 0 : 1; channel < 2; channel++)
	{
		unsigned int count;

		enabled[channel] = op2(m, OP_I_NOT_EQUAL, m->t_bool, op2(m, OP_BITWISE_AND, m->t_int, mask,
			constant_int(m, 1 << channel)), constant_int(m, 0));
		active[channel] = op2(m, OP_LOGICAL_OR, m->t_bool, load(m, m->t_bool, variables[channel]),
			op2(m, OP_LOGICAL_AND, m->t_bool, enabled[channel],
			compare(m, OP_F_ORD_GREATER_THAN_EQUAL, d[channel], constant_float(m, 0.0f))));
		store(m, variables[channel], active[channel]);
		count = op2(m, OP_LOGICAL_AND, m->t_bool, enabled[channel], active[channel]);
		nearest = fselect(m, count, ext2(m, GLSL_F_MIN, m->t_float, nearest, d[channel]), nearest);
		inside = fselect(m, op2(m, OP_LOGICAL_AND, m->t_bool, enabled[channel],
			op1(m, OP_LOGICAL_NOT, m->t_bool, active[channel])),
			ext2(m, GLSL_F_MIN, m->t_float, inside, fneg(m, 1, d[channel])), inside);
	}
	is_hit = compare(m, OP_F_ORD_LESS_THAN, nearest, fmul(m, 1, constant_float(m, XGPU_WORLD_TRACE_HIT), voxel));
	kind = constant_float(m, (float)XGPU_WORLD_TRACE_OBJECTS);
	if (field)
		kind = fselect(m, op2(m, OP_LOGICAL_AND, m->t_bool, op2(m, OP_LOGICAL_AND, m->t_bool, enabled[0], active[0]),
			compare(m, OP_F_ORD_LESS_THAN_EQUAL, d[0], nearest)), constant_float(m, (float)XGPU_WORLD_TRACE_STRUCTURE),
			kind);
	selection_begin(m, is_hit, &hit_merge);
	store(m, hit_variable, constant_float(m, 1.0f));
	store(m, distance_variable, t);
	store(m, kind_variable, kind);
	selection_end(m, hit_merge);
	advance = ext2(m, GLSL_F_MAX, m->t_float, ext2(m, GLSL_F_MIN, m->t_float, nearest, inside),
		fmul(m, 1, constant_float(m, XGPU_WORLD_TRACE_LEAST_STEP), voxel));
	t_next = fadd(m, 1, t, advance);
	store(m, t_variable, t_next);
	store(m, done_variable, op2(m, OP_LOGICAL_OR, m->t_bool, is_hit,
		compare(m, OP_F_ORD_GREATER_THAN_EQUAL, t_next, t_max)));
	selection_end(m, found_merge);
	branch(m, next);
	label_at(m, next);
	store(m, step_variable, op2(m, OP_I_ADD, m->t_int, load(m, m->t_int, step_variable), constant_int(m, 1)));
	branch(m, header);
	label_at(m, merge);
	result[0] = load(m, m->t_float, hit_variable);
	result[1] = load(m, m->t_float, distance_variable);
	result[2] = load(m, m->t_float, kind_variable);
}

/* the cascades' texture's variable (XGPU_SPIRV_SET_OCCLUSION,
XGPU_SPIRV_BINDING_WORLD_CASCADES) */
static unsigned int world_cascades_declare(struct module *m)
{
	unsigned int texture = variable(m, type_sampled_image(m, DIM_3D, FALSE, FALSE, TRUE), STORAGE_UNIFORM_CONSTANT,
		"world_cascades");

	decorate1(m, texture, DECORATION_DESCRIPTOR_SET, XGPU_SPIRV_SET_OCCLUSION);
	decorate1(m, texture, DECORATION_BINDING, XGPU_SPIRV_BINDING_WORLD_CASCADES);
	return texture;
}

BOOL xgpu_spirv_world_trace_test(struct xgpu_spirv_module *module)
{
	struct module m;
	struct words locals = { 0 };
	unsigned int main_id, texture, origin_in, direction_in, parameters_in, cascades_in[XGPU_WORLD_CASCADES], color_out;
	unsigned int cascades[XGPU_WORLD_CASCADES], parameters, result[3], field_in[2], field[2];
	int cascade;

	module_begin(&m, EXECUTION_MODEL_FRAGMENT);
	texture = world_cascades_declare(&m);
	world_sdf_declare(&m);
	origin_in = interface_variable(&m, m.t_v3, STORAGE_INPUT, 0, "origin");
	direction_in = interface_variable(&m, m.t_v3, STORAGE_INPUT, 1, "direction");
	parameters_in = interface_variable(&m, m.t_v4, STORAGE_INPUT, 2, "parameters");
	decorate(&m, parameters_in, DECORATION_FLAT, 0, NULL);
	for (cascade = 0; cascade < XGPU_WORLD_CASCADES; cascade++)
	{
		static const char *const names[XGPU_WORLD_CASCADES] = { "cascade0", "cascade1", "cascade2", "cascade3" };

		cascades_in[cascade] = interface_variable(&m, m.t_v4, STORAGE_INPUT, 3 + cascade, names[cascade]);
		decorate(&m, cascades_in[cascade], DECORATION_FLAT, 0, NULL);
	}
	field_in[0] = interface_variable(&m, m.t_v4, STORAGE_INPUT, 3 + XGPU_WORLD_CASCADES, "field0");
	field_in[1] = interface_variable(&m, m.t_v4, STORAGE_INPUT, 4 + XGPU_WORLD_CASCADES, "field1");
	decorate(&m, field_in[0], DECORATION_FLAT, 0, NULL);
	decorate(&m, field_in[1], DECORATION_FLAT, 0, NULL);
	color_out = interface_variable(&m, m.t_v4, STORAGE_OUTPUT, 0, "fragment_color");
	main_id = function_begin(&m);
	for (cascade = 0; cascade < XGPU_WORLD_CASCADES; cascade++)
		cascades[cascade] = load(&m, m.t_v4, cascades_in[cascade]);
	parameters = load(&m, m.t_v4, parameters_in);
	field[0] = load(&m, m.t_v4, field_in[0]);
	field[1] = load(&m, m.t_v4, field_in[1]);
	world_trace(&m, &locals, texture, cascades, field, load(&m, m.t_v3, origin_in), load(&m, m.t_v3, direction_in),
		extract(&m, parameters, 0), op1(&m, OP_CONVERT_F_TO_S, m.t_int, extract(&m, parameters, 1)), result);
	store(&m, color_out, construct4(&m, result[0], result[1], result[2], constant_float(&m, 0.0f)));
	return module_end(&m, EXECUTION_MODEL_FRAGMENT, main_id, &locals, module);
}

/* the occlusion at the pixel, from its world position and normal, for the
light it multiplies (XGPU_AMBIENT_OCCLUSION_LIGHT_*): a model's, the
field's relative to its reference faded in, times the others' proxies; the
lightmap pass's, the proxies alone; then the floor and the strength
(xgpu.h) */
static unsigned int world_ao_read(struct module *m, struct pixel_state *state, int light)
{
	unsigned int p = pixel_input(m, state, state->in_world_position, m->t_v3);
	unsigned int normal = xyz(m, pixel_input(m, state, state->in_world_normal, m->t_v4));
	unsigned int n = fscale(m, 3, normal, ext1(m, GLSL_INVERSE_SQRT, m->t_float,
		fmax_scalar(m, 1, fdot(m, normal, normal), 1.0e-24f)));
	unsigned int object = uniform(m, U_WORLD_AO, -1);
	unsigned int parameters0 = world_ao_vector(m, 1, constant_int(m, 0));
	unsigned int parameters1 = world_ao_vector(m, 1, constant_int(m, 1));
	/* (the shapes walked: the dynamic objects', parameters[1].y of them;
	every one in the lightmap pass where its page has no layer, or it reads
	no lightmap: world_ao_shading.c's world_ao_proxies_walked) */
	unsigned int dynamic = op1(m, OP_CONVERT_F_TO_S, m->t_int, extract(m, parameters1, 1));
	unsigned int layer = extract(m, object, 1), limit = dynamic, scenery = 0, proxies;
	unsigned int field = 0, relative = 0, occlusion;
	int paged = light == XGPU_AMBIENT_OCCLUSION_LIGHT_LIGHTMAP && state->in_t[2] &&
		state->program->stages[2].mode == XGPU_TEXTURE_PROJECT2D && state->program->stages[2].sampler == _xgpu_sampler_2d;

	if (light == XGPU_AMBIENT_OCCLUSION_LIGHT_LIGHTMAP)
	{
		unsigned int unpaged = compare(m, OP_F_ORD_LESS_THAN, layer, constant_float(m, 0.0f));

		if (paged)
		{
			/* the page's texel at the lightmap's own coordinates, as stage 2
			samples them: xT2.xy / (xT2.w != 0 ? xT2.w : 1) by its scale */
			unsigned int xt = pixel_input(m, state, state->in_t[2], m->t_v4), w = extract(m, xt, 3), uv, operands[4];

			w = op3(m, OP_SELECT, m->t_float, compare(m, OP_F_UNORD_NOT_EQUAL, w, constant_float(m, 0.0f)), w,
				constant_float(m, 1.0f));
			uv = fmul(m, 2, fdiv(m, 2, xy(m, xt), splat(m, 2, w)), xy(m, uniform(m, U_TEXTURE_SCALE, 2)));
			operands[0] = load(m, type_sampled_image(m, DIM_2D, TRUE, FALSE, TRUE), m->world_ao_scenery);
			operands[1] = op(m, OP_COMPOSITE_CONSTRUCT, m->t_v3, 3, (unsigned int[]){ extract(m, uv, 0),
				extract(m, uv, 1), fmax_scalar(m, 1, layer, 0.0f) });
			operands[2] = IMAGE_OPERANDS_LOD;
			operands[3] = constant_float(m, 0.0f);
			/* (the pages' bytes are the coverage's format, read as 1, 1, 1, the
			byte: their alpha) */
			scenery = fselect(m, unpaged, constant_float(m, 1.0f),
				extract(m, op(m, OP_IMAGE_SAMPLE_EXPLICIT_LOD, m->t_v4, 4, operands), 3));
			limit = op3(m, OP_SELECT, m->t_int, unpaged, constant_int(m, XGPU_WORLD_AO_SHAPES), dynamic);
		}
		else
		{
			limit = constant_int(m, XGPU_WORLD_AO_SHAPES);
		}
	}
	/* (a model's field: where its reference is negative, a static scenery
	placement's own draw, the field holds its own geometry, so its taps are
	taken from the pixel's own depth in it, relative to no reference; a
	two-sided shader's cards, its world_ao.w not 0, about their most open
	direction, the shapes too) */
	if (light != XGPU_AMBIENT_OCCLUSION_LIGHT_LIGHTMAP)
	{
		unsigned int baked = compare(m, OP_F_ORD_LESS_THAN, extract(m, object, 1), constant_float(m, 0.0f));
		unsigned int field0 = world_ao_vector(m, 0, constant_int(m, 0)), field1 = world_ao_vector(m, 0, constant_int(m, 1));
		unsigned int inside = ext2(m, GLSL_F_MIN, m->t_float, world_sdf_distance(m, &state->locals, field0, field1, p),
			constant_float(m, 0.0f));
		unsigned int reference = fselect(m, baked, constant_float(m, 1.0f), extract(m, object, 1));

		field = world_sdf_cards_occlusion(m, state, p, &n, fselect(m, baked, inside, constant_float(m, 0.0f)),
			compare(m, OP_F_UNORD_NOT_EQUAL, extract(m, object, 3), constant_float(m, 0.0f)));
		relative = saturate(m, 1, fdiv(m, 1, field, fmax_scalar(m, 1, reference, XGPU_WORLD_AO_REFERENCE_LEAST)));
	}
	proxies = world_proxies_occlusion(m, state, p, n, extract(m, object, 0), limit, constant_int(m, 0), 0);
	if (light == XGPU_AMBIENT_OCCLUSION_LIGHT_LIGHTMAP)
	{
		if (scenery)
			proxies = fmul(m, 1, proxies, scenery);
		return ext3(m, GLSL_F_MIX, m->t_float, constant_float(m, 1.0f), fmax_scalar(m, 1, proxies, XGPU_WORLD_AO_FLOOR),
			extract(m, parameters0, 1));
	}
	field = ext3(m, GLSL_F_MIX, m->t_float, field, relative, extract(m, parameters1, 0));
	occlusion = fmul(m, 1, ext3(m, GLSL_F_MIX, m->t_float, constant_float(m, 1.0f), field, extract(m, parameters0, 2)),
		proxies);
	return ext3(m, GLSL_F_MIX, m->t_float, constant_float(m, 1.0f), fmax_scalar(m, 1, occlusion, XGPU_WORLD_AO_FLOOR),
		extract(m, parameters0, 0));
}

/* found once a pixel: a pre-pass draw's inputs to its pixel's occlusion
(xgpu.h's XGPU_AMBIENT_OCCLUSION_WORLD_INPUTS; xgpu_msl.c's
world_ao_inputs, world_ao_shading.c's world_ao_inputs_pack), two words */
static unsigned int world_ao_inputs(struct module *m, struct pixel_state *state, int light)
{
	unsigned int normal = xyz(m, pixel_input(m, state, state->in_world_normal, m->t_v4));
	unsigned int n = fscale(m, 3, normal, ext1(m, GLSL_INVERSE_SQRT, m->t_float,
		fmax_scalar(m, 1, fdot(m, normal, normal), 1.0e-24f)));
	unsigned int object = uniform(m, U_WORLD_AO, -1);
	unsigned int value = extract(m, object, 1), flags, folded, flipped, negative, positive, words[4];
	unsigned int absolute = ext1(m, GLSL_F_ABS, m->t_v3, n);
	int paged = light == XGPU_AMBIENT_OCCLUSION_LIGHT_LIGHTMAP && state->in_t[2] &&
		state->program->stages[2].mode == XGPU_TEXTURE_PROJECT2D && state->program->stages[2].sampler == _xgpu_sampler_2d;

	/* flags = (uint(int(owner) + 2) & 0xFF) | present */
	flags = op2(m, OP_BITWISE_OR, m->t_uint, op2(m, OP_BITWISE_AND, m->t_uint, op1(m, OP_BITCAST, m->t_uint,
		op2(m, OP_I_ADD, m->t_int, op1(m, OP_CONVERT_F_TO_S, m->t_int, extract(m, object, 0)), constant_int(m, 2))),
		constant_uint(m, 0xFFu)), constant_uint(m, XGPU_WORLD_AO_INPUT_PRESENT));
	if (paged)
	{
		/* (the page's texel, as world_ao_read reads it; every shape where
		the page has no layer) */
		unsigned int unpaged = compare(m, OP_F_ORD_LESS_THAN, value, constant_float(m, 0.0f));
		unsigned int xt = pixel_input(m, state, state->in_t[2], m->t_v4), w = extract(m, xt, 3), uv, operands[4], texel;

		w = op3(m, OP_SELECT, m->t_float, compare(m, OP_F_UNORD_NOT_EQUAL, w, constant_float(m, 0.0f)), w,
			constant_float(m, 1.0f));
		uv = fmul(m, 2, fdiv(m, 2, xy(m, xt), splat(m, 2, w)), xy(m, uniform(m, U_TEXTURE_SCALE, 2)));
		operands[0] = load(m, type_sampled_image(m, DIM_2D, TRUE, FALSE, TRUE), m->world_ao_scenery);
		operands[1] = op(m, OP_COMPOSITE_CONSTRUCT, m->t_v3, 3, (unsigned int[]){ extract(m, uv, 0),
			extract(m, uv, 1), fmax_scalar(m, 1, value, 0.0f) });
		operands[2] = IMAGE_OPERANDS_LOD;
		operands[3] = constant_float(m, 0.0f);
		texel = extract(m, op(m, OP_IMAGE_SAMPLE_EXPLICIT_LOD, m->t_v4, 4, operands), 3);
		value = fselect(m, unpaged, constant_float(m, 1.0f), texel);
		flags = op2(m, OP_BITWISE_OR, m->t_uint, flags, op3(m, OP_SELECT, m->t_uint, unpaged,
			constant_uint(m, XGPU_WORLD_AO_INPUT_LIGHTMAP | XGPU_WORLD_AO_INPUT_ALL_SHAPES),
			constant_uint(m, XGPU_WORLD_AO_INPUT_LIGHTMAP)));
	}
	else if (light == XGPU_AMBIENT_OCCLUSION_LIGHT_LIGHTMAP)
	{
		value = constant_float(m, 1.0f);
		flags = op2(m, OP_BITWISE_OR, m->t_uint, flags,
			constant_uint(m, XGPU_WORLD_AO_INPUT_LIGHTMAP | XGPU_WORLD_AO_INPUT_ALL_SHAPES));
	}
	/* (a two-sided model shader's cards, its world_ao.w not 0: found about
	their most open direction) */
	flags = op2(m, OP_BITWISE_OR, m->t_uint, flags, op3(m, OP_SELECT, m->t_uint, compare(m, OP_F_UNORD_NOT_EQUAL,
		extract(m, object, 3), constant_float(m, 0.0f)), constant_uint(m, XGPU_WORLD_AO_INPUT_CARDS), constant_uint(m, 0)));
	/* octahedral: n.xy / (|x| + |y| + |z|), the lower half folded over the
	diagonals: (1 - |folded.yx|) * (folded >= 0 ? 1 : -1) */
	folded = fdiv(m, 2, xy(m, n), splat(m, 2, fmax_scalar(m, 1, fadd(m, 1, fadd(m, 1, extract(m, absolute, 0),
		extract(m, absolute, 1)), extract(m, absolute, 2)), 1.0e-24f)));
	positive = op2(m, OP_F_ORD_GREATER_THAN_EQUAL, m->t_bv2, folded, constant_splat(m, 2, 0.0f));
	flipped = fmul(m, 2, fsub(m, 2, constant_splat(m, 2, 1.0f), ext1(m, GLSL_F_ABS, m->t_v2,
		op(m, OP_VECTOR_SHUFFLE, m->t_v2, 4, (unsigned int[]){ folded, folded, 1, 0 }))),
		op3(m, OP_SELECT, m->t_v2, positive, constant_splat(m, 2, 1.0f), constant_splat(m, 2, -1.0f)));
	negative = compare(m, OP_F_ORD_LESS_THAN, extract(m, n, 2), constant_float(m, 0.0f));
	folded = select_n(m, 2, negative, flipped, folded);
	words[0] = ext1(m, GLSL_PACK_SNORM_2X16, m->t_uint, folded);
	words[1] = op2(m, OP_BITWISE_OR, m->t_uint, flags, op2(m, OP_SHIFT_LEFT_LOGICAL, m->t_uint,
		ext1(m, GLSL_PACK_HALF_2X16, m->t_uint, op(m, OP_COMPOSITE_CONSTRUCT, m->t_v2, 2,
		(unsigned int[]){ value, constant_float(m, 0.0f) })), constant_uint(m, 16)));
	/* (its distance from the eye, the grid's middle) */
	words[2] = op1(m, OP_BITCAST, m->t_uint, flength(m, fsub(m, 3, pixel_input(m, state, state->in_world_position, m->t_v3),
		xyz(m, world_ao_vector(m, 3, constant_int(m, 0))))));
	/* (with "ray_traced", its object's owner among the ray-traced scene's
	instances: 0 none) */
	words[3] = op1(m, OP_BITCAST, m->t_uint, op1(m, OP_CONVERT_F_TO_S, m->t_int, extract(m, object, 2)));
	return op(m, OP_COMPOSITE_CONSTRUCT, m->t_uv4, 4, words);
}

/* found once a pixel: a surface's pixel's occlusion read from the pass's
texels (xgpu_msl.c's world_ao_upsample, world_ao_shading.c's): at a
quarter of the pixels (the block's parameters[1].w, 2), the four about it
by their bilinear weights and their distances' and normals' nearness to its
own; at each pixel (1), its own */
static unsigned int world_ao_unfold(struct module *m, unsigned int packed)
{
	unsigned int absolute = ext1(m, GLSL_F_ABS, m->t_v2, packed), z, t, folded;

	z = fsub(m, 1, fsub(m, 1, constant_float(m, 1.0f), extract(m, absolute, 0)), extract(m, absolute, 1));
	t = fmax_scalar(m, 1, fneg(m, 1, z), 0.0f);
	folded = fadd(m, 2, packed, op3(m, OP_SELECT, m->t_v2, op2(m, OP_F_ORD_GREATER_THAN_EQUAL, m->t_bv2, packed,
		constant_splat(m, 2, 0.0f)), splat(m, 2, fneg(m, 1, t)), splat(m, 2, t)));
	return ext1(m, GLSL_NORMALIZE, m->t_v3, op(m, OP_COMPOSITE_CONSTRUCT, m->t_v3, 3,
		(unsigned int[]){ extract(m, folded, 0), extract(m, folded, 1), z }));
}

static unsigned int world_ao_pixels_fetch(struct module *m, unsigned int image, unsigned int at)
{
	unsigned int operands[4];

	operands[0] = image;
	operands[1] = at;
	operands[2] = IMAGE_OPERANDS_LOD;
	operands[3] = constant_int(m, 0);
	return op(m, OP_IMAGE_FETCH, m->t_v4, 4, operands);
}

/* a texel's closeness to the pixel (world_ao_shading.c's
world_ao_upsample_closeness) */
static unsigned int world_ao_closeness(struct module *m, unsigned int texel, unsigned int distance, unsigned int facing,
	unsigned int n)
{
	unsigned int texel_distance = extract(m, texel, 1);
	unsigned int texel_n = world_ao_unfold(m, op(m, OP_VECTOR_SHUFFLE, m->t_v2, 4, (unsigned int[]){ texel, texel, 2, 3 }));
	unsigned int relative = fmul(m, 1, fmul(m, 1, ext1(m, GLSL_F_ABS, m->t_float, fsub(m, 1, fdiv(m, 1, texel_distance,
		distance), constant_float(m, 1.0f))), facing), constant_float(m, 1.0f / XGPU_WORLD_AO_UPSAMPLE_DEPTH));
	unsigned int along = fmax_scalar(m, 1, fdot(m, n, texel_n), 0.0f);

	along = fmul(m, 1, along, along);
	along = fmul(m, 1, along, along);
	along = fmul(m, 1, along, along);
	return fselect(m, op2(m, OP_LOGICAL_AND, m->t_bool, compare(m, OP_F_ORD_GREATER_THAN, texel_distance,
		constant_float(m, 0.0f)), compare(m, OP_F_ORD_GREATER_THAN, distance, constant_float(m, 0.0f))),
		fdiv(m, 1, along, fadd(m, 1, constant_float(m, 1.0f), fmul(m, 1, relative, relative))), constant_float(m, 0.0f));
}

static unsigned int world_ao_upsample(struct module *m, struct pixel_state *state)
{
	unsigned int sampled = load(m, type_sampled_image(m, DIM_2D, FALSE, FALSE, TRUE), m->world_ao_pixels);
	unsigned int image = op(m, OP_IMAGE, type_sampled_image(m, DIM_2D, FALSE, FALSE, FALSE), 1, &sampled);
	unsigned int frag = load(m, m->t_v4, state->frag_coord), scale = extract(m, world_ao_vector(m, 1, constant_int(m, 1)), 3);
	unsigned int own = extract(m, world_ao_pixels_fetch(m, image, op1(m, OP_CONVERT_F_TO_S, m->t_iv2, xy(m, frag))), 0);
	unsigned int normal = xyz(m, pixel_input(m, state, state->in_world_normal, m->t_v4));
	unsigned int n = fscale(m, 3, normal, ext1(m, GLSL_INVERSE_SQRT, m->t_float,
		fmax_scalar(m, 1, fdot(m, normal, normal), 1.0e-24f)));
	unsigned int ray = fsub(m, 3, pixel_input(m, state, state->in_world_position, m->t_v3),
		xyz(m, world_ao_vector(m, 3, constant_int(m, 0))));
	unsigned int distance = flength(m, ray);
	unsigned int facing = fmax_scalar(m, 1, fdiv(m, 1, ext1(m, GLSL_F_ABS, m->t_float, fdot(m, n, ray)),
		fmax_scalar(m, 1, distance, 1.0e-12f)), XGPU_WORLD_AO_UPSAMPLE_FACING);
	unsigned int at = fsub(m, 2, fdiv(m, 2, xy(m, frag), splat(m, 2, fmax_scalar(m, 1, scale, 1.0f))),
		constant_splat(m, 2, 0.5f));
	unsigned int base = ext1(m, GLSL_FLOOR, m->t_v2, at), f = fsub(m, 2, at, base);
	unsigned int last = op2(m, OP_I_SUB, m->t_iv2, image_size(m, image, m->t_iv2),
		op(m, OP_COMPOSITE_CONSTRUCT, m->t_iv2, 2, (unsigned int[]){ constant_int(m, 1), constant_int(m, 1) }));
	unsigned int ibase = op1(m, OP_CONVERT_F_TO_S, m->t_iv2, base), zero2 = op(m, OP_COMPOSITE_CONSTRUCT, m->t_iv2, 2,
		(unsigned int[]){ constant_int(m, 0), constant_int(m, 0) });
	unsigned int sum = constant_float(m, 0.0f), weights = constant_float(m, 0.0f), result, merge, close;
	unsigned int result_variable = local(m, &state->locals, m->t_float);
	unsigned int best_variable = local(m, &state->locals, m->t_float);
	int i, j;

	for (j = 0; j < 2; j++)
	{
		for (i = 0; i < 2; i++)
		{
			unsigned int t = ext3(m, GLSL_S_CLAMP, m->t_iv2, op2(m, OP_I_ADD, m->t_iv2, ibase,
				op(m, OP_COMPOSITE_CONSTRUCT, m->t_iv2, 2, (unsigned int[]){ constant_int(m, i), constant_int(m, j) })),
				zero2, last);
			unsigned int texel = world_ao_pixels_fetch(m, image, t);
			unsigned int weight = fmul(m, 1, fmul(m, 1, i ? extract(m, f, 0) : fsub(m, 1, constant_float(m, 1.0f),
				extract(m, f, 0)), j ? extract(m, f, 1) : fsub(m, 1, constant_float(m, 1.0f), extract(m, f, 1))),
				world_ao_closeness(m, texel, distance, facing, n));

			sum = fadd(m, 1, sum, fmul(m, 1, weight, extract(m, texel, 0)));
			weights = fadd(m, 1, weights, weight);
		}
	}
	close = compare(m, OP_F_ORD_GREATER_THAN, weights, constant_float(m, XGPU_WORLD_AO_UPSAMPLE_LEAST));
	store(m, result_variable, fdiv(m, 1, sum, fmax_scalar(m, 1, weights, 1.0e-30f)));
	/* (none close: the closest of the 4x4 about it) */
	selection_begin(m, op1(m, OP_LOGICAL_NOT, m->t_bool, close), &merge);
	store(m, result_variable, constant_float(m, 1.0f));
	store(m, best_variable, constant_float(m, -1.0f));
	for (j = -1; j < 3; j++)
	{
		for (i = -1; i < 3; i++)
		{
			unsigned int t = ext3(m, GLSL_S_CLAMP, m->t_iv2, op2(m, OP_I_ADD, m->t_iv2, ibase,
				op(m, OP_COMPOSITE_CONSTRUCT, m->t_iv2, 2, (unsigned int[]){ constant_int(m, i), constant_int(m, j) })),
				zero2, last);
			unsigned int texel = world_ao_pixels_fetch(m, image, t);
			unsigned int closeness = world_ao_closeness(m, texel, distance, facing, n);
			unsigned int better = compare(m, OP_F_ORD_GREATER_THAN, closeness, load(m, m->t_float, best_variable));

			store(m, result_variable, fselect(m, better, extract(m, texel, 0), load(m, m->t_float, result_variable)));
			store(m, best_variable, fselect(m, better, closeness, load(m, m->t_float, best_variable)));
		}
	}
	selection_end(m, merge);
	result = load(m, m->t_float, result_variable);
	return fselect(m, compare(m, OP_F_ORD_LESS_THAN_EQUAL, scale, constant_float(m, 1.0f)), own, result);
}

/* ---------- per-pixel model lighting */

static unsigned int model_lighting(struct module *m, struct pixel_state *state, BOOL point_lights, BOOL dynamic_lights,
	unsigned int occlusion)
{
	unsigned int world_normal = pixel_input(m, state, state->in_world_normal, m->t_v4);
	unsigned int normal = xyz(m, world_normal), n, facing, light;
	int index;

	/* n = xWorldNormal.xyz * (xWorldNormal.w * inversesqrt(max(dot(...), 1.0e-24))); with a normal map,
	its normal at the vertex's length (xWorldNormal.w) */
	if (state->shaded_normal)
		n = fscale(m, 3, state->shaded_normal, extract(m, world_normal, 3));
	else
		n = fscale(m, 3, normal, fmul(m, 1, extract(m, world_normal, 3), ext1(m, GLSL_INVERSE_SQRT, m->t_float,
			fmax_scalar(m, 1, fdot(m, normal, normal), (float)1.0e-24))));
	facing = fdot(m, n, fneg(m, 3, xyz(m, uniform(m, U_MODEL_LIGHTS, 7))));
	/* light = ml[11].xyz + max(max(facing, -facing * ml[0].z), 0.0) * ml[8].xyz + max(dot(n, -ml[9].xyz), 0.0) * ml[10].xyz */
	light = fadd(m, 3, fadd(m, 3, xyz(m, uniform(m, U_MODEL_LIGHTS, 11)),
		fscale(m, 3, xyz(m, uniform(m, U_MODEL_LIGHTS, 8)), fmax_scalar(m, 1, ext2(m, GLSL_F_MAX, m->t_float, facing,
		fmul(m, 1, fneg(m, 1, facing), extract(m, uniform(m, U_MODEL_LIGHTS, 0), 2))), 0.0f))),
		fscale(m, 3, xyz(m, uniform(m, U_MODEL_LIGHTS, 10)), fmax_scalar(m, 1,
		fdot(m, n, fneg(m, 3, xyz(m, uniform(m, U_MODEL_LIGHTS, 9)))), 0.0f)));
	/* (display.ambient_occlusion: the ambient and distant lights, the
	level's light about the model, by the occlusion; not the point lights) */
	if (occlusion)
		light = fscale(m, 3, light, occlusion);
	for (index = 0; point_lights && !dynamic_lights && index < 2; index++)
	{
		int first = 1 + index * 3;
		unsigned int position = pixel_input(m, state, state->in_world_position, m->t_v3);
		unsigned int first_light = uniform(m, U_MODEL_LIGHTS, first), cone = uniform(m, U_MODEL_LIGHTS, first + 1);
		unsigned int color = uniform(m, U_MODEL_LIGHTS, first + 2);
		unsigned int to_light = fsub(m, 3, xyz(m, first_light), position);
		unsigned int distance_squared = fdot(m, to_light, to_light);
		unsigned int l = fscale(m, 3, to_light, ext1(m, GLSL_INVERSE_SQRT, m->t_float,
			fmax_scalar(m, 1, distance_squared, (float)1.0e-12)));
		unsigned int term;

		/* max(1.0 - d2 * w, 0.0) * clamp(dot(l, n), 0, 1) * clamp(dot(l, -cone.xyz) * cone.w + color.w, 0, 1) * color.xyz */
		term = fmul(m, 1, fmul(m, 1, fmax_scalar(m, 1, fsub(m, 1, constant_float(m, 1.0f),
			fmul(m, 1, distance_squared, extract(m, first_light, 3))), 0.0f), saturate(m, 1, fdot(m, l, n))),
			saturate(m, 1, fadd(m, 1, fmul(m, 1, fdot(m, l, fneg(m, 3, xyz(m, cone))), extract(m, cone, 3)), extract(m, color, 3))));
		light = fadd(m, 3, light, fscale(m, 3, xyz(m, color), term));
	}
	return saturate(m, 3, light);
}

/* ---------- the enhanced dynamic lights (xgpu.h) */

static void declare_dynamic_lights(struct module *m)
{
	unsigned int members[4], block, cells_type;
	int member;

	members[0] = block_array(m, 3);
	for (member = 1; member < 4; member++)
		members[member] = block_array(m, XGPU_DYNAMIC_LIGHTS_MAXIMUM);
	block = type_struct(m, 4, members);
	decorate(m, block, DECORATION_BLOCK, 0, NULL);
	member_decorate(m, block, 0, DECORATION_OFFSET, (unsigned int)offsetof(struct xgpu_dynamic_lights_block, grid));
	member_decorate(m, block, 1, DECORATION_OFFSET, (unsigned int)offsetof(struct xgpu_dynamic_lights_block, position));
	member_decorate(m, block, 2, DECORATION_OFFSET, (unsigned int)offsetof(struct xgpu_dynamic_lights_block, color));
	member_decorate(m, block, 3, DECORATION_OFFSET, (unsigned int)offsetof(struct xgpu_dynamic_lights_block, cone));
	name(m, block, "dynamic_lights_block");
	m->dynamic_lights = variable(m, block, STORAGE_UNIFORM, "dynamic_lights");
	decorate1(m, m->dynamic_lights, DECORATION_DESCRIPTOR_SET, XGPU_SPIRV_SET_DRAW);
	decorate1(m, m->dynamic_lights, DECORATION_BINDING, XGPU_SPIRV_BINDING_DYNAMIC_LIGHTS);
	cells_type = type_sampled_image(m, DIM_3D, FALSE, TRUE, TRUE);
	m->dynamic_light_cells = variable(m, cells_type, STORAGE_UNIFORM_CONSTANT, "dynamic_light_cells");
	decorate1(m, m->dynamic_light_cells, DECORATION_DESCRIPTOR_SET, XGPU_SPIRV_SET_DRAW);
	decorate1(m, m->dynamic_light_cells, DECORATION_BINDING, XGPU_SPIRV_BINDING_DYNAMIC_LIGHT_CELLS);
}

/* an element of the block's member (0 grid, 1 position, 2 color, 3 cone):
a constant index or an int value */
static unsigned int dynamic_light(struct module *m, int member, unsigned int index_id)
{
	unsigned int operands[3], pointer;

	operands[0] = m->dynamic_lights;
	operands[1] = constant_int(m, member);
	operands[2] = index_id;
	pointer = op(m, OP_ACCESS_CHAIN, type_pointer(m, STORAGE_UNIFORM, m->t_v4), 3, operands);
	return load(m, m->t_v4, pointer);
}

/* a texel of the cells' texture (xgpu.h): the cells, the clusters' masks at
z = GRID_Z, the world past them */
static unsigned int dynamic_light_texel(struct module *m, unsigned int coordinates)
{
	unsigned int sampled = load(m, type_sampled_image(m, DIM_3D, FALSE, TRUE, TRUE), m->dynamic_light_cells);
	unsigned int image = op(m, OP_IMAGE, type_sampled_image(m, DIM_3D, FALSE, TRUE, FALSE), 1, &sampled);
	unsigned int operands[4];

	operands[0] = image;
	operands[1] = coordinates;
	operands[2] = IMAGE_OPERANDS_LOD;
	operands[3] = constant_int(m, 0);
	return op(m, OP_IMAGE_FETCH, m->t_uv4, 4, operands);
}

/* the world's texel t (a uint, at most its last: xgpu.h) */
static unsigned int dynamic_light_world_texel(struct module *m, unsigned int t)
{
	unsigned int x, y, z;

	t = ext2(m, GLSL_U_MIN, m->t_uint, t, constant_uint(m, XGPU_DYNAMIC_LIGHTS_WORLD_TEXELS - 1));
	x = op2(m, OP_BITWISE_AND, m->t_uint, t, constant_uint(m, 31));
	y = op2(m, OP_BITWISE_AND, m->t_uint, op2(m, OP_SHIFT_RIGHT_LOGICAL, m->t_uint, t, constant_uint(m, 5)),
		constant_uint(m, 31));
	z = op2(m, OP_I_ADD, m->t_uint, op2(m, OP_SHIFT_RIGHT_LOGICAL, m->t_uint, t, constant_uint(m, 10)),
		constant_uint(m, XGPU_DYNAMIC_LIGHTS_GRID_Z + 1));
	return dynamic_light_texel(m, op1(m, OP_BITCAST, m->t_iv3,
		op(m, OP_COMPOSITE_CONSTRUCT, type_vector(m, m->t_uint, 3), 3, (unsigned int[]){ x, y, z })));
}

static unsigned int uextract(struct module *m, unsigned int vector, unsigned int component)
{
	return op2(m, OP_COMPOSITE_EXTRACT, m->t_uint, vector, component);
}

/* the cell's lights that reach the point q's cluster (xgpu.h): its
cluster's mask, the world's tree walked down from its root to q's cluster,
in a loop of SPIR-V's structure; every light where the world has no nodes,
or puts q in no cluster. Only where the cell has a light whose walk could
take it away (XGPU_DYNAMIC_LIGHTS_WALK: a model's, or the structure's):
mask is the cell's */
static unsigned int dynamic_light_cluster_mask(struct module *m, struct pixel_state *state, unsigned int mask,
	unsigned int q, BOOL environment)
{
	unsigned int all = constant_uint(m, 0xffffffffu), top = constant_uint(m, 0x80000000u);
	unsigned int header = dynamic_light_world_texel(m, constant_uint(m, 0));
	unsigned int node_count = uextract(m, header, 0), root = uextract(m, header, 1);
	unsigned int mask_variable = local(m, &state->locals, m->t_uv4);
	unsigned int node_variable = local(m, &state->locals, m->t_uint);
	unsigned int leaf_variable = local(m, &state->locals, m->t_uint);
	unsigned int depth_variable = local(m, &state->locals, m->t_int);
	unsigned int walk, after, loop_header, loop_condition, loop_body, loop_continue, loop_merge;
	unsigned int node, texel, plane, children, distance, child, is_leaf, leaf, cluster, found, cluster_mask;
	unsigned int walked, zero, condition;

	store(m, mask_variable, mask);
	/* (the lights the world is walked for: their texel past the clusters'
	masks) */
	walked = dynamic_light_texel(m, op(m, OP_COMPOSITE_CONSTRUCT, m->t_iv3, 3, (unsigned int[]){
		constant_int(m, (XGPU_DYNAMIC_LIGHTS_WALK + (environment ? 1 : 0)) % 32),
		constant_int(m, (XGPU_DYNAMIC_LIGHTS_WALK + (environment ? 1 : 0)) / 32),
		constant_int(m, XGPU_DYNAMIC_LIGHTS_GRID_Z) }));
	zero = op(m, OP_COMPOSITE_CONSTRUCT, m->t_uv4, 4, (unsigned int[]){ constant_uint(m, 0), constant_uint(m, 0),
		constant_uint(m, 0), constant_uint(m, 0) });
	condition = op2(m, OP_LOGICAL_AND, m->t_bool, op1(m, OP_ANY, m->t_bool, op2(m, OP_I_NOT_EQUAL, m->t_bv4,
		op2(m, OP_BITWISE_AND, m->t_uv4, mask, walked), zero)),
		op2(m, OP_I_NOT_EQUAL, m->t_bool, node_count, constant_uint(m, 0)));
	walk = id_new(m);
	after = id_new(m);
	selection_merge(m, after);
	branch_conditional(m, condition, walk, after);
	label_at(m, walk);
	store(m, node_variable, root);
	store(m, leaf_variable, all);
	store(m, depth_variable, constant_int(m, 0));

	loop_header = id_new(m);
	loop_condition = id_new(m);
	loop_body = id_new(m);
	loop_continue = id_new(m);
	loop_merge = id_new(m);
	branch(m, loop_header);
	label_at(m, loop_header);
	{
		unsigned int operands[3] = { loop_merge, loop_continue, 0 };

		op_void(m, OP_LOOP_MERGE, 3, operands);
	}
	branch(m, loop_condition);
	label_at(m, loop_condition);
	/* while (depth < DEPTH && node < node_count) */
	node = load(m, m->t_uint, node_variable);
	branch_conditional(m, op2(m, OP_LOGICAL_AND, m->t_bool,
		op2(m, OP_S_LESS_THAN, m->t_bool, load(m, m->t_int, depth_variable), constant_int(m, XGPU_DYNAMIC_LIGHTS_WORLD_DEPTH)),
		op2(m, OP_U_LESS_THAN, m->t_bool, node, node_count)), loop_body, loop_merge);
	label_at(m, loop_body);
	/* plane = world[1 + 2 node]; children = world[2 + 2 node] (the two reads
	apart); child = dot(plane.xyz, q) - plane.w >= 0.0 ? children.y : children.x */
	texel = op2(m, OP_I_ADD, m->t_uint, op2(m, OP_I_MUL, m->t_uint, node, constant_uint(m, 2)), constant_uint(m, 1));
	plane = op1(m, OP_BITCAST, m->t_v4, dynamic_light_world_texel(m, texel));
	children = dynamic_light_world_texel(m, op2(m, OP_I_ADD, m->t_uint, texel, constant_uint(m, 1)));
	distance = fsub(m, 1, fdot(m, xyz(m, plane), q), extract(m, plane, 3));
	child = op(m, OP_SELECT, m->t_uint, 3, (unsigned int[]){ compare(m, OP_F_ORD_GREATER_THAN_EQUAL, distance,
		constant_float(m, 0.0f)), uextract(m, children, 1), uextract(m, children, 0) });
	is_leaf = op2(m, OP_I_NOT_EQUAL, m->t_bool, op2(m, OP_BITWISE_AND, m->t_uint, child, top), constant_uint(m, 0));
	/* a cluster (or none) ends the walk */
	store(m, leaf_variable, op(m, OP_SELECT, m->t_uint, 3, (unsigned int[]){ is_leaf, child, all }));
	store(m, node_variable, op(m, OP_SELECT, m->t_uint, 3, (unsigned int[]){ is_leaf, all, child }));
	branch(m, loop_continue);
	label_at(m, loop_continue);
	store(m, depth_variable, op2(m, OP_I_ADD, m->t_int, load(m, m->t_int, depth_variable), constant_int(m, 1)));
	branch(m, loop_header);
	label_at(m, loop_merge);

	/* the cluster's mask */
	leaf = load(m, m->t_uint, leaf_variable);
	found = op2(m, OP_I_NOT_EQUAL, m->t_bool, leaf, all);
	cluster = op2(m, OP_BITWISE_AND, m->t_uint, leaf, constant_uint(m, XGPU_DYNAMIC_LIGHTS_CLUSTERS - 1));
	cluster_mask = dynamic_light_texel(m, op1(m, OP_BITCAST, m->t_iv3, op(m, OP_COMPOSITE_CONSTRUCT,
		type_vector(m, m->t_uint, 3), 3, (unsigned int[]){ op2(m, OP_BITWISE_AND, m->t_uint, cluster, constant_uint(m, 31)),
		op2(m, OP_SHIFT_RIGHT_LOGICAL, m->t_uint, cluster, constant_uint(m, 5)),
		constant_uint(m, XGPU_DYNAMIC_LIGHTS_GRID_Z) })));
	store(m, mask_variable, op(m, OP_SELECT, m->t_uv4, 3, (unsigned int[]){
		op(m, OP_COMPOSITE_CONSTRUCT, m->t_bv4, 4, (unsigned int[]){ found, found, found, found }),
		op2(m, OP_BITWISE_AND, m->t_uv4, mask, cluster_mask), mask }));
	branch(m, after);
	label_at(m, after);
	return load(m, m->t_uv4, mask_variable);
}

/* ---------- display.shadows "ray_traced": the dynamic lights' shadows, as
the surfaces read them (xgpu.h's XGPU_DYNAMIC_LIGHTS_SHADOWED)

A surface found once a pixel (display.ambient_occlusion "world"'s sampled
draws: drawn after the pre-pass, where the mask is traced) with the
enhanced lights reads, before its loop of them, the mask's texel at the
pixel (XGPU_SPIRV_SET_OCCLUSION, XGPU_SPIRV_BINDING_LIGHT_SHADOWS: a 1x1 of 1
where none is traced) and which lights its channels are (the cells'
texel); each light of its cell is multiplied by its channel, a light no
channel is by 1. As GLSL:

uvec4 shadowed = texelFetch(cells, ivec3(SHADOWED % 32, SHADOWED / 32, GRID_Z), 0);
vec4 seen = texelFetch(light_shadows, min(ivec2(gl_FragCoord.xy), textureSize(light_shadows, 0) - 1), 0);
...
float shadow = 1.0;
for (int c = 0; c < 4; c++) shadow = shadowed[c] == uint(light + 1) ? seen[c] : shadow;
falloff *= shadow; */

static void light_shadows_declare(struct module *m, struct pixel_state *state)
{
	state->light_shadows = variable(m, type_sampled_image(m, DIM_2D, FALSE, FALSE, TRUE), STORAGE_UNIFORM_CONSTANT,
		"light_shadows");
	decorate1(m, state->light_shadows, DECORATION_DESCRIPTOR_SET, XGPU_SPIRV_SET_OCCLUSION);
	decorate1(m, state->light_shadows, DECORATION_BINDING, XGPU_SPIRV_BINDING_LIGHT_SHADOWS);
}

/* the cells' texel n past XGPU_DYNAMIC_LIGHTS_SHADOWED (xgpu.h) */
static unsigned int light_shadows_texel(struct module *m, int n)
{
	int texel = XGPU_DYNAMIC_LIGHTS_SHADOWED + n;

	return dynamic_light_texel(m, op(m, OP_COMPOSITE_CONSTRUCT, m->t_iv3, 3, (unsigned int[]){
		constant_int(m, texel % XGPU_DYNAMIC_LIGHTS_GRID_X), constant_int(m, texel / XGPU_DYNAMIC_LIGHTS_GRID_X),
		constant_int(m, XGPU_DYNAMIC_LIGHTS_GRID_Z) }));
}

/* the pixel's mask and its channels' lights, read once before the loop
(shadowed 0: none read, no light shadowed) */
struct light_shadows_read
{
	unsigned int shadowed, seen, stand_in;
};

static int light_shadows_view(void);

static void light_shadows_begin(struct module *m, struct pixel_state *state, struct light_shadows_read *read)
{
	unsigned int sampled, image, frag, size, at, operands[4];

	memset(read, 0, sizeof(*read));
	if (!state->light_shadows || !state->frag_coord)
		return;
	read->shadowed = light_shadows_texel(m, 0);
	sampled = load(m, type_sampled_image(m, DIM_2D, FALSE, FALSE, TRUE), state->light_shadows);
	image = op(m, OP_IMAGE, type_sampled_image(m, DIM_2D, FALSE, FALSE, FALSE), 1, &sampled);
	frag = load(m, m->t_v4, state->frag_coord);
	size = op2(m, OP_I_SUB, m->t_iv2, image_size(m, image, m->t_iv2), op(m, OP_COMPOSITE_CONSTRUCT, m->t_iv2, 2,
		(unsigned int[]){ constant_int(m, 1), constant_int(m, 1) }));
	at = ext2(m, GLSL_S_MIN, m->t_iv2, op1(m, OP_CONVERT_F_TO_S, m->t_iv2, xy(m, frag)), size);
	operands[0] = image;
	operands[1] = at;
	operands[2] = IMAGE_OPERANDS_LOD;
	operands[3] = constant_int(m, 0);
	read->seen = op(m, OP_IMAGE_FETCH, m->t_v4, 4, operands);
	/* (the view: whether it is the stand-in, 1x1) */
	if (light_shadows_view())
		read->stand_in = op2(m, OP_I_EQUAL, m->t_bool, op2(m, OP_COMPOSITE_EXTRACT, m->t_int, size, 0), constant_int(m, 0));
}

/* (HALO_LIGHT_SHADOWS_VIEW=1, checking: each shadowed light drawn in its
channel's tint where its disk is seen, red where it is not, its falloff
whole; the other lights as they are) */
static int light_shadows_view(void)
{
	static int on = -1;

	if (on < 0)
	{
		const char *view = getenv("HALO_LIGHT_SHADOWS_VIEW");

		on = view && *view && strcmp(view, "0") ? (atoi(view) > 1 ? atoi(view) : 1) : 0;
	}
	return on;
}

/* a light's falloff (light: its index, an int) times its channel; with
the view, the falloff whole, and in *tint (0 none: not shadowed) the
colour it is drawn in */
static unsigned int light_shadows_factor(struct module *m, const struct light_shadows_read *read, unsigned int light,
	unsigned int falloff, unsigned int *tint)
{
	static const float tints[XGPU_LIGHT_SHADOWS_CHANNELS][3] = { { 0.0f, 2.0f, 0.0f }, { 0.0f, 0.6f, 2.5f },
		{ 1.6f, 1.6f, 0.0f }, { 0.0f, 1.6f, 1.6f } };
	unsigned int number, shadow = constant_float(m, 1.0f), matched = 0, color = 0;
	int channel;

	*tint = 0;
	if (!read->shadowed)
		return falloff;
	number = op2(m, OP_I_ADD, m->t_uint, op1(m, OP_BITCAST, m->t_uint, light), constant_uint(m, 1));
	for (channel = 0; channel < XGPU_LIGHT_SHADOWS_CHANNELS; channel++)
	{
		unsigned int is = op2(m, OP_I_EQUAL, m->t_bool, uextract(m, read->shadowed, (unsigned int)channel), number);

		shadow = fselect(m, is, extract(m, read->seen, channel), shadow);
		if (!light_shadows_view())
			continue;
		matched = matched ? op2(m, OP_LOGICAL_OR, m->t_bool, matched, is) : is;
		color = select_n(m, 3, is, constant_vector(m, 3, tints[channel]), color ? color :
			constant_splat(m, 3, 0.0f));
	}
	if (!light_shadows_view())
		return fmul(m, 1, falloff, shadow);
	/* (the stand-in read: magenta) */
	color = select_n(m, 3, read->stand_in, constant_vector(m, 3, (const float[]){ 2.0f, 0.0f, 2.0f }), color);
	/* tint * seen + red * (1 - seen), where a channel is the light's */
	*tint = vec4_of(m, fadd(m, 3, fscale(m, 3, color, shadow), fscale(m, 3, constant_vector(m, 3,
		(const float[]){ 3.0f, 0.0f, 0.0f }), fsub(m, 1, constant_float(m, 1.0f), shadow))),
		fselect(m, matched, constant_float(m, 1.0f), constant_float(m, 0.0f)));
	return falloff;
}

/* ---------- the classic light model (display.ray_traced_lighting off:
port/linux/game/lt_lights.h): the enhanced dynamic lights as they were
before the ray-traced lighting's light model, unchanged */

/* dynamic_light_sum(p, n, surface_normal, environment): the lights of the
pixel's cell, summed (a model's but those its object's own that do not light
it, its model_lights[XGPU_MODEL_LIGHT_OWN_LIGHTS]); its loops are SPIR-V's structured loops, its values
kept in the function's variables. With a reflection (a model's, of the eye
about its normal; 0 none), each light's highlight along it, to the exponent,
summed into the shine variable as well (xgpu.h XGPU_DYNAMIC_LIGHTS_HIGHLIGHT;
xgpu_msl.c's dynamic_light_sum) */
static unsigned int dynamic_light_sum_classic(struct module *m, struct pixel_state *state, unsigned int p, unsigned int n,
	unsigned int surface_normal, BOOL environment, unsigned int reflection, unsigned int exponent,
	unsigned int shine_variable)
{
	unsigned int grid0 = dynamic_light(m, 0, constant_int(m, 0)), grid1 = dynamic_light(m, 0, constant_int(m, 1));
	unsigned int offset, cell, mask, cell_index, operands[4];
	unsigned int sum_variable, word_variable, bits_variable;
	unsigned int outer_header, outer_condition, outer_body, outer_continue, outer_merge;
	unsigned int inner_header, inner_condition, inner_body, inner_continue, inner_merge;
	unsigned int word, bits, light, position, to_light, distance_squared, x2, l, window, core, falloff, facing;
	unsigned int cone, color, spot, grid2, w, own = 0, tint = 0;
	struct light_shadows_read shadows;

	/* cell = sign(offset) * log2(1.0 + abs(offset) * grid[1].w) * grid[1].xyz + vec3(16.0, 16.0, 8.0) */
	offset = fsub(m, 3, p, xyz(m, grid0));
	cell = fadd(m, 3, fmul(m, 3, fmul(m, 3, ext1(m, GLSL_F_SIGN, m->t_v3, offset),
		ext1(m, GLSL_LOG2, m->t_v3, fadd(m, 3, constant_splat(m, 3, 1.0f),
		fscale(m, 3, ext1(m, GLSL_F_ABS, m->t_v3, offset), extract(m, grid1, 3))))), xyz(m, grid1)),
		constant_vector(m, 3, (const float[]){ (float)(XGPU_DYNAMIC_LIGHTS_GRID_X / 2), (float)(XGPU_DYNAMIC_LIGHTS_GRID_Y / 2),
		(float)(XGPU_DYNAMIC_LIGHTS_GRID_Z / 2) }));
	cell_index = ext3(m, GLSL_S_CLAMP, m->t_iv3, op1(m, OP_CONVERT_F_TO_S, m->t_iv3, ext1(m, GLSL_FLOOR, m->t_v3, cell)),
		op(m, OP_COMPOSITE_CONSTRUCT, m->t_iv3, 3, (unsigned int[]){ constant_int(m, 0), constant_int(m, 0), constant_int(m, 0) }),
		op(m, OP_COMPOSITE_CONSTRUCT, m->t_iv3, 3, (unsigned int[]){ constant_int(m, XGPU_DYNAMIC_LIGHTS_GRID_X - 1),
		constant_int(m, XGPU_DYNAMIC_LIGHTS_GRID_Y - 1), constant_int(m, XGPU_DYNAMIC_LIGHTS_GRID_Z - 1) }));
	mask = dynamic_light_texel(m, cell_index);
	/* (those of the cell's lights that reach the pixel's cluster, its point
	a little off its surface: xgpu.h) */
	mask = dynamic_light_cluster_mask(m, state, mask, fadd(m, 3, p, fscale(m, 3, surface_normal,
		constant_float(m, XGPU_DYNAMIC_LIGHTS_CLUSTER_OFFSET))), environment);
	grid2 = dynamic_light(m, 0, constant_int(m, 2));
	/* (a model's: the lights it is not lit by, xgpu.h) */
	if (!environment)
		own = uniform(m, U_MODEL_LIGHTS, XGPU_MODEL_LIGHT_OWN_LIGHTS);
	light_shadows_begin(m, state, &shadows);

	sum_variable = local(m, &state->locals, m->t_v3);
	word_variable = local(m, &state->locals, m->t_int);
	bits_variable = local(m, &state->locals, m->t_uint);
	store(m, sum_variable, constant_splat(m, 3, 0.0f));
	store(m, word_variable, constant_int(m, 0));

	outer_header = id_new(m);
	outer_condition = id_new(m);
	outer_body = id_new(m);
	outer_continue = id_new(m);
	outer_merge = id_new(m);
	branch(m, outer_header);
	label_at(m, outer_header);
	operands[0] = outer_merge;
	operands[1] = outer_continue;
	operands[2] = 0;
	op_void(m, OP_LOOP_MERGE, 3, operands);
	branch(m, outer_condition);
	label_at(m, outer_condition);
	word = load(m, m->t_int, word_variable);
	branch_conditional(m, op2(m, OP_S_LESS_THAN, m->t_bool, word, constant_int(m, 4)), outer_body, outer_merge);
	label_at(m, outer_body);
	store(m, bits_variable, op2(m, OP_VECTOR_EXTRACT_DYNAMIC, m->t_uint, mask, word));

	inner_header = id_new(m);
	inner_condition = id_new(m);
	inner_body = id_new(m);
	inner_continue = id_new(m);
	inner_merge = id_new(m);
	branch(m, inner_header);
	label_at(m, inner_header);
	operands[0] = inner_merge;
	operands[1] = inner_continue;
	operands[2] = 0;
	op_void(m, OP_LOOP_MERGE, 3, operands);
	branch(m, inner_condition);
	label_at(m, inner_condition);
	bits = load(m, m->t_uint, bits_variable);
	branch_conditional(m, op2(m, OP_I_NOT_EQUAL, m->t_bool, bits, constant_uint(m, 0)), inner_body, inner_merge);
	label_at(m, inner_body);
	/* light = word * 32 + findLSB(bits); bits &= bits - 1u */
	light = op2(m, OP_I_ADD, m->t_int, op2(m, OP_I_MUL, m->t_int, word, constant_int(m, 32)),
		ext1(m, GLSL_FIND_I_LSB, m->t_int, bits));
	store(m, bits_variable, op2(m, OP_BITWISE_AND, m->t_uint, bits, op2(m, OP_I_SUB, m->t_uint, bits, constant_uint(m, 1))));
	position = dynamic_light(m, 1, light);
	w = extract(m, position, 3);
	if (environment)
	{
		unsigned int skip = id_new(m), condition = compare(m, OP_F_ORD_LESS_THAN, w, constant_float(m, 0.0f));

		/* if (environment && position.w < 0.0) continue; */
		selection_merge(m, skip);
		branch_conditional(m, condition, inner_continue, skip);
		label_at(m, skip);
	}
	else
	{
		unsigned int skip = id_new(m), index = op1(m, OP_CONVERT_S_TO_F, m->t_float, light), condition;
		int slot;

		/* if (any(own == vec4(float(light)))) continue; */
		condition = compare(m, OP_F_ORD_EQUAL, extract(m, own, 0), index);
		for (slot = 1; slot < 4; slot++)
			condition = op2(m, OP_LOGICAL_OR, m->t_bool, condition, compare(m, OP_F_ORD_EQUAL, extract(m, own, slot), index));
		selection_merge(m, skip);
		branch_conditional(m, condition, inner_continue, skip);
		label_at(m, skip);
	}
	to_light = fsub(m, 3, xyz(m, position), p);
	distance_squared = fdot(m, to_light, to_light);
	x2 = fmul(m, 1, fmul(m, 1, distance_squared, w), w);
	{
		unsigned int skip = id_new(m), condition = compare(m, OP_F_ORD_GREATER_THAN_EQUAL, x2, constant_float(m, 1.0f));

		/* if (x2 >= 1.0) continue; */
		selection_merge(m, skip);
		branch_conditional(m, condition, inner_continue, skip);
		label_at(m, skip);
	}
	l = fscale(m, 3, to_light, ext1(m, GLSL_INVERSE_SQRT, m->t_float, fmax_scalar(m, 1, distance_squared, (float)1.0e-8)));
	window = fsub(m, 1, constant_float(m, 1.0f), fmul(m, 1, x2, x2));
	core = fmax_scalar(m, 1, fmul(m, 1, fmul(m, 1, extract(m, grid2, 1), w), w),
		(float)(1.0 / (XGPU_DYNAMIC_LIGHTS_CORE * XGPU_DYNAMIC_LIGHTS_CORE)));
	falloff = fdiv(m, 1, fmul(m, 1, window, window), fadd(m, 1, constant_float(m, 1.0f), fmul(m, 1, distance_squared, core)));
	falloff = light_shadows_factor(m, &shadows, light, falloff, &tint);
	if (environment)
	{
		facing = fmul(m, 1, fmax_scalar(m, 1, fdot(m, n, l), 0.0f),
			saturate(m, 1, fadd(m, 1, fmul(m, 1, fdot(m, surface_normal, l), constant_float(m, 4.0f)), constant_float(m, 0.5f))));
	}
	else
	{
		unsigned int wrap = extract(m, grid2, 3);

		facing = saturate(m, 1, fdiv(m, 1, fadd(m, 1, fdot(m, n, l), wrap), fadd(m, 1, constant_float(m, 1.0f), wrap)));
	}
	cone = dynamic_light(m, 3, light);
	color = dynamic_light(m, 2, light);
	/* (HALO_LIGHT_SHADOWS_VIEW: a shadowed light in its tint) */
	if (tint)
		color = vec4_of(m, select_n(m, 3, compare(m, OP_F_ORD_GREATER_THAN, extract(m, tint, 3), constant_float(m, 0.5f)),
			xyz(m, tint), xyz(m, color)), extract(m, color, 3));
	spot = saturate(m, 1, fadd(m, 1, fmul(m, 1, fdot(m, l, fneg(m, 3, xyz(m, cone))), extract(m, cone, 3)), extract(m, color, 3)));
	store(m, sum_variable, fadd(m, 3, load(m, m->t_v3, sum_variable),
		fscale(m, 3, xyz(m, color), fmul(m, 1, fmul(m, 1, falloff, facing), spot))));
	if (reflection)
	{
		/* shine += color.rgb * (falloff * spot * saturate(dot(n, l) * 4.0) * pow(max(dot(reflection, l), 0.0), exponent)) */
		unsigned int lobe = ext2(m, GLSL_POW, m->t_float, fmax_scalar(m, 1, fdot(m, reflection, l), 0.0f), exponent);
		unsigned int horizon = saturate(m, 1, fmul(m, 1, fdot(m, n, l), constant_float(m, 4.0f)));

		store(m, shine_variable, fadd(m, 3, load(m, m->t_v3, shine_variable),
			fscale(m, 3, xyz(m, color), fmul(m, 1, fmul(m, 1, fmul(m, 1, falloff, spot), horizon), lobe))));
	}
	branch(m, inner_continue);
	label_at(m, inner_continue);
	branch(m, inner_header);
	label_at(m, inner_merge);
	branch(m, outer_continue);
	label_at(m, outer_continue);
	store(m, word_variable, op2(m, OP_I_ADD, m->t_int, load(m, m->t_int, word_variable), constant_int(m, 1)));
	branch(m, outer_header);
	label_at(m, outer_merge);
	return load(m, m->t_v3, sum_variable);
}

/* dynamic_light_add(base, light): the light added to the base under the
shoulder, in linear space; the base as it is (saturated) where there is no
light, as the shoulder gives it, but for its rounding there and back */
static unsigned int dynamic_light_add_classic(struct module *m, struct pixel_state *state, unsigned int base,
	unsigned int light, BOOL hdr)
{
	unsigned int grid2, brightest, had, room, gamma, lit, added, after, any_light, result_variable;

	result_variable = local(m, &state->locals, m->t_v3);
	store(m, result_variable, saturate(m, 3, base));
	any_light = op1(m, OP_ANY, m->t_bool, op2(m, OP_F_UNORD_NOT_EQUAL, m->t_bv3, light,
		constant_splat(m, 3, 0.0f)));
	added = id_new(m);
	after = id_new(m);
	selection_merge(m, after);
	branch_conditional(m, any_light, added, after);
	label_at(m, added);
	grid2 = dynamic_light(m, 0, constant_int(m, 2));
	brightest = ext2(m, GLSL_F_MAX, m->t_float, extract(m, light, 0), ext2(m, GLSL_F_MAX, m->t_float, extract(m, light, 1),
		extract(m, light, 2)));
	light = fadd(m, 3, light, splat(m, 3, fmul(m, 1, fmax_scalar(m, 1, fsub(m, 1, brightest, constant_float(m, 1.0f)), 0.0f),
		extract(m, grid2, 2))));
	had = ext2(m, GLSL_POW, m->t_v3, saturate(m, 3, base), constant_splat(m, 3, 2.2f));
	room = fmax_scalar(m, 3, fsub(m, 3, constant_splat(m, 3, 1.0f), had), (float)1.0e-3);
	gamma = constant_splat(m, 3, (float)(1.0 / 2.2));
	lit = fadd(m, 3, had, fmul(m, 3, room, fsub(m, 3, constant_splat(m, 3, 1.0f),
		ext1(m, GLSL_EXP, m->t_v3, fdiv(m, 3, fneg(m, 3, light), room)))));
	/* (display.hdr: the same below white, and what its shoulder squeezes
	under white rolled off toward the lights' white instead: xgpu_hdr.h's
	xgpu_hdr_lights, xgpu_msl.c's dynamic_light_add_hdr) */
	if (hdr)
	{
		unsigned int past = fmax_scalar(m, 3, fsub(m, 3, fadd(m, 3, had, light), constant_splat(m, 3, 1.0f)), 0.0f);
		unsigned int white_room = constant_splat(m, 3, XGPU_HDR_LIGHTS_WHITE - 1.0f);

		lit = fadd(m, 3, lit, fmul(m, 3, white_room, fsub(m, 3, constant_splat(m, 3, 1.0f),
			ext1(m, GLSL_EXP, m->t_v3, fdiv(m, 3, fneg(m, 3, past), white_room)))));
	}
	store(m, result_variable, ext2(m, GLSL_POW, m->t_v3, lit, gamma));
	branch(m, after);
	label_at(m, after);
	return load(m, m->t_v3, result_variable);
}

/* ---------- the enhanced flashlight (xgpu.h's XGPU_DYNAMIC_LIGHTS_FLASHLIGHT)

A light the slots name is lit through its gel, as the game's gel pass
projects it, its own falloff, and, for the shadowed slot, its shadow map,
in place of its cone and the lights' falloff. As GLSL:

uvec4 slots = texel(FLASHLIGHT), head = texel(FLASHLIGHT + 1);
if (any(slots == uvec4(light + 1))) {
  int slot = the first that is; vec3 f, u, s = the slot's forward, up, left;
  vec3 v = p - light.xyz; float z = dot(v, f), R = uintBitsToFloat(head.y);
  vec2 ab = vec2(dot(v, s), dot(v, u)) / max(z, 1e-4) / R;
  spot = z > 0 && all(lessThan(abs(ab), vec2(1))) ? pow(cookie(ab * 0.5 + 0.5), 2.2) : 0;
  falloff = profile(length(v) * abs(light.w));   (32 values, linear between)
  environment: facing = max(dot(n, l), 0) * saturate(dot(surface_normal, l) * 8)   (the gel pass's horizon)
  if (head.x == slot + 1)   (the map's from the lamp: ab and v of p - lamp)
    spot *= the map's 3x3 tent of comparisons at ab: lit where the map's distance >= length(v) - bias,
      bias = uintBitsToFloat(head.w) + length(v) * (2 R / SIZE) * 1.5 / max(dot(surface_normal, l), 0.25)
} */

/* the clusters' slice's texel at an index (an int) */
static unsigned int flashlight_texel(struct module *m, unsigned int index)
{
	return dynamic_light_texel(m, op(m, OP_COMPOSITE_CONSTRUCT, m->t_iv3, 3, (unsigned int[]){
		op2(m, OP_BITWISE_AND, m->t_int, index, constant_int(m, XGPU_DYNAMIC_LIGHTS_GRID_X - 1)),
		op2(m, OP_SHIFT_RIGHT_ARITHMETIC, m->t_int, index, constant_int(m, 5)),
		constant_int(m, XGPU_DYNAMIC_LIGHTS_GRID_Z) }));
}

static unsigned int flashlight_vector(struct module *m, unsigned int index)
{
	return xyz(m, op1(m, OP_BITCAST, m->t_v4, flashlight_texel(m, index)));
}

static unsigned int int_add(struct module *m, unsigned int a, int b)
{
	return op2(m, OP_I_ADD, m->t_int, a, constant_int(m, b));
}

/* the cookie's byte at (x, y), ints within it, 0 to 1 */
static unsigned int flashlight_cookie_byte(struct module *m, unsigned int x, unsigned int y)
{
	unsigned int k = op2(m, OP_I_ADD, m->t_int, op2(m, OP_I_MUL, m->t_int, y,
		constant_int(m, XGPU_DYNAMIC_LIGHTS_FLASHLIGHT_COOKIE_SIZE)), x);
	unsigned int texel = flashlight_texel(m, int_add(m, op2(m, OP_SHIFT_RIGHT_ARITHMETIC, m->t_int, k,
		constant_int(m, 4)), XGPU_DYNAMIC_LIGHTS_FLASHLIGHT_COOKIE));
	unsigned int word = op2(m, OP_VECTOR_EXTRACT_DYNAMIC, m->t_uint, texel, op2(m, OP_BITWISE_AND, m->t_int,
		op2(m, OP_SHIFT_RIGHT_ARITHMETIC, m->t_int, k, constant_int(m, 2)), constant_int(m, 3)));
	unsigned int shift = op1(m, OP_BITCAST, m->t_uint, op2(m, OP_SHIFT_LEFT_LOGICAL, m->t_int,
		op2(m, OP_BITWISE_AND, m->t_int, k, constant_int(m, 3)), constant_int(m, 3)));
	unsigned int byte = op2(m, OP_BITWISE_AND, m->t_uint, op2(m, OP_SHIFT_RIGHT_LOGICAL, m->t_uint, word, shift),
		constant_uint(m, 255));

	return fmul(m, 1, op1(m, OP_CONVERT_U_TO_F, m->t_float, byte), constant_float(m, 1.0f / 255.0f));
}

/* a bilinear lookup at (x, y), texel units from the first's middle, of a
grid size square: the four texels' ints (clamped) and weights */
struct flashlight_bilinear
{
	unsigned int x0, y0, x1, y1, fx, fy;
};

static void flashlight_bilinear(struct module *m, unsigned int x, unsigned int y, int size,
	struct flashlight_bilinear *b)
{
	unsigned int floor_x = ext1(m, GLSL_FLOOR, m->t_float, x), floor_y = ext1(m, GLSL_FLOOR, m->t_float, y);
	unsigned int ix = op1(m, OP_CONVERT_F_TO_S, m->t_int, floor_x), iy = op1(m, OP_CONVERT_F_TO_S, m->t_int, floor_y);
	unsigned int zero = constant_int(m, 0), last = constant_int(m, size - 1);

	b->fx = fsub(m, 1, x, floor_x);
	b->fy = fsub(m, 1, y, floor_y);
	b->x0 = ext3(m, GLSL_S_CLAMP, m->t_int, ix, zero, last);
	b->y0 = ext3(m, GLSL_S_CLAMP, m->t_int, iy, zero, last);
	b->x1 = ext3(m, GLSL_S_CLAMP, m->t_int, int_add(m, ix, 1), zero, last);
	b->y1 = ext3(m, GLSL_S_CLAMP, m->t_int, int_add(m, iy, 1), zero, last);
}

static unsigned int flashlight_mix(struct module *m, unsigned int a, unsigned int b, unsigned int t)
{
	return ext3(m, GLSL_F_MIX, m->t_float, a, b, t);
}

static void flashlight_shadow_declare(struct module *m)
{
	if (m->flashlight_shadow)
		return;
	m->flashlight_shadow = variable(m, type_sampled_image(m, DIM_2D, FALSE, FALSE, TRUE), STORAGE_UNIFORM_CONSTANT,
		"flashlight_shadow");
	decorate1(m, m->flashlight_shadow, DECORATION_DESCRIPTOR_SET, XGPU_SPIRV_SET_DRAW);
	decorate1(m, m->flashlight_shadow, DECORATION_BINDING, XGPU_SPIRV_BINDING_FLASHLIGHT_SHADOW);
}

/* the map's comparisons at (x, y), texel units: the share lit of its four
texels about it, bilinear */
static unsigned int flashlight_shadow_compare(struct module *m, unsigned int image, unsigned int x, unsigned int y,
	unsigned int receiver)
{
	struct flashlight_bilinear b;
	unsigned int lit[4], operands[4];
	int corner;

	flashlight_bilinear(m, x, y, XGPU_DYNAMIC_LIGHTS_FLASHLIGHT_SHADOW_SIZE, &b);
	for (corner = 0; corner < 4; corner++)
	{
		operands[0] = image;
		operands[1] = op(m, OP_COMPOSITE_CONSTRUCT, m->t_iv2, 2, (unsigned int[]){ corner & 1 ? b.x1 : b.x0,
			corner & 2 ? b.y1 : b.y0 });
		operands[2] = IMAGE_OPERANDS_LOD;
		operands[3] = constant_int(m, 0);
		lit[corner] = fselect(m, compare(m, OP_F_ORD_GREATER_THAN_EQUAL, extract(m, op(m, OP_IMAGE_FETCH, m->t_v4, 4,
			operands), 0), receiver), constant_float(m, 1.0f), constant_float(m, 0.0f));
	}
	return flashlight_mix(m, flashlight_mix(m, lit[0], lit[1], b.fx), flashlight_mix(m, lit[2], lit[3], b.fx), b.fy);
}

/* read once before the loop of the lights */
struct flashlight_read
{
	unsigned int slots, head;
};

static void flashlight_begin(struct module *m, struct flashlight_read *read)
{
	read->slots = flashlight_texel(m, constant_int(m, XGPU_DYNAMIC_LIGHTS_FLASHLIGHT));
	read->head = flashlight_texel(m, constant_int(m, XGPU_DYNAMIC_LIGHTS_FLASHLIGHT + 1));
}

/* every light's falloff on the structure (the game's distance
attenuation, xgpu.h's XGPU_DYNAMIC_LIGHTS_ATTENUATION) at a distance, of a
light whose w is 1 / its radius (its sign the models-only's) */
static unsigned int light_attenuation(struct module *m, unsigned int distance, unsigned int w)
{
	unsigned int t, f, index, low, high;

	t = fmul(m, 1, saturate(m, 1, fmul(m, 1, distance, ext1(m, GLSL_F_ABS, m->t_float, w))),
		constant_float(m, (float)(XGPU_DYNAMIC_LIGHTS_FLASHLIGHT_FALLOFF - 1)));
	f = ext1(m, GLSL_FLOOR, m->t_float, t);
	index = ext3(m, GLSL_S_CLAMP, m->t_int, op1(m, OP_CONVERT_F_TO_S, m->t_int, f), constant_int(m, 0),
		constant_int(m, XGPU_DYNAMIC_LIGHTS_FLASHLIGHT_FALLOFF - 1));
	low = op1(m, OP_BITCAST, m->t_float, op2(m, OP_VECTOR_EXTRACT_DYNAMIC, m->t_uint, flashlight_texel(m, int_add(m,
		op2(m, OP_SHIFT_RIGHT_ARITHMETIC, m->t_int, index, constant_int(m, 2)), XGPU_DYNAMIC_LIGHTS_ATTENUATION)),
		op2(m, OP_BITWISE_AND, m->t_int, index, constant_int(m, 3))));
	index = ext2(m, GLSL_S_MIN, m->t_int, int_add(m, index, 1), constant_int(m, XGPU_DYNAMIC_LIGHTS_FLASHLIGHT_FALLOFF - 1));
	high = op1(m, OP_BITCAST, m->t_float, op2(m, OP_VECTOR_EXTRACT_DYNAMIC, m->t_uint, flashlight_texel(m, int_add(m,
		op2(m, OP_SHIFT_RIGHT_ARITHMETIC, m->t_int, index, constant_int(m, 2)), XGPU_DYNAMIC_LIGHTS_ATTENUATION)),
		op2(m, OP_BITWISE_AND, m->t_int, index, constant_int(m, 3))));
	return flashlight_mix(m, low, high, fsub(m, 1, t, f));
}

/* a light of the loop (an int), at p (l and position as the loop has
them): where it is a flashlight's, on the structure its spot (1, a
point's) by its gel, as the game's gel pass multiplies it, and on every
surface, for the shadowed slot, by its shadow map; the spot's variable
changed in place (the falloff's by the checking views alone) */
static void flashlight_light(struct module *m, const struct flashlight_read *read, unsigned int light, unsigned int p,
	unsigned int position, unsigned int l, unsigned int surface_normal, BOOL environment, BOOL shadowed,
	unsigned int falloff_variable, unsigned int spot_variable)
{
	unsigned int number = op1(m, OP_BITCAST, m->t_uint, int_add(m, light, 1)), is[4], any, slot, merge;
	unsigned int base, forward, up, left, v, z, width, inverse, a, b, inside, x, y, cookie, distance;
	struct flashlight_bilinear bl;
	int k;

	for (k = 0; k < 4; k++)
		is[k] = op2(m, OP_I_EQUAL, m->t_bool, uextract(m, read->slots, (unsigned int)k), number);
	any = op2(m, OP_LOGICAL_OR, m->t_bool, op2(m, OP_LOGICAL_OR, m->t_bool, is[0], is[1]),
		op2(m, OP_LOGICAL_OR, m->t_bool, is[2], is[3]));
	selection_begin(m, any, &merge);
	slot = op3(m, OP_SELECT, m->t_int, is[0], constant_int(m, 0), op3(m, OP_SELECT, m->t_int, is[1], constant_int(m, 1),
		op3(m, OP_SELECT, m->t_int, is[2], constant_int(m, 2), constant_int(m, 3))));
	base = int_add(m, op2(m, OP_I_MUL, m->t_int, slot, constant_int(m, 3)), XGPU_DYNAMIC_LIGHTS_FLASHLIGHT + 2);
	forward = flashlight_vector(m, base);
	up = flashlight_vector(m, int_add(m, base, 1));
	left = flashlight_vector(m, int_add(m, base, 2));
	v = fsub(m, 3, p, xyz(m, position));
	z = fdot(m, v, forward);
	width = op1(m, OP_BITCAST, m->t_float, uextract(m, read->head, 1));
	inverse = fdiv(m, 1, constant_float(m, 1.0f), fmul(m, 1, fmax_scalar(m, 1, z, 1.0e-4f), fmax_scalar(m, 1, width,
		1.0e-4f)));
	a = fmul(m, 1, fdot(m, v, left), inverse);
	b = fmul(m, 1, fdot(m, v, up), inverse);
	inside = op2(m, OP_LOGICAL_AND, m->t_bool, compare(m, OP_F_ORD_GREATER_THAN, z, constant_float(m, 1.0e-4f)),
		op2(m, OP_LOGICAL_AND, m->t_bool,
		compare(m, OP_F_ORD_LESS_THAN, ext1(m, GLSL_F_ABS, m->t_float, a), constant_float(m, 1.0f)),
		compare(m, OP_F_ORD_LESS_THAN, ext1(m, GLSL_F_ABS, m->t_float, b), constant_float(m, 1.0f))));
	if (environment)
	{
		/* (the cookie, bilinear: texel (x, y) at u = sign(a) sqrt(|a|)'s (2 x + 1) / N - 1, xgpu.h; as it
		is, the display's gamma, as the gel pass multiplies it) */
		x = fmul(m, 1, ext1(m, GLSL_F_SIGN, m->t_float, a), fsqrt(m, ext1(m, GLSL_F_ABS, m->t_float, a)));
		y = fmul(m, 1, ext1(m, GLSL_F_SIGN, m->t_float, b), fsqrt(m, ext1(m, GLSL_F_ABS, m->t_float, b)));
		x = fsub(m, 1, fmul(m, 1, fadd(m, 1, fmul(m, 1, x, constant_float(m, 0.5f)), constant_float(m, 0.5f)),
			constant_float(m, (float)XGPU_DYNAMIC_LIGHTS_FLASHLIGHT_COOKIE_SIZE)), constant_float(m, 0.5f));
		y = fsub(m, 1, fmul(m, 1, fadd(m, 1, fmul(m, 1, y, constant_float(m, 0.5f)), constant_float(m, 0.5f)),
			constant_float(m, (float)XGPU_DYNAMIC_LIGHTS_FLASHLIGHT_COOKIE_SIZE)), constant_float(m, 0.5f));
		flashlight_bilinear(m, x, y, XGPU_DYNAMIC_LIGHTS_FLASHLIGHT_COOKIE_SIZE, &bl);
		cookie = flashlight_mix(m, flashlight_mix(m, flashlight_cookie_byte(m, bl.x0, bl.y0),
			flashlight_cookie_byte(m, bl.x1, bl.y0), bl.fx), flashlight_mix(m, flashlight_cookie_byte(m, bl.x0, bl.y1),
			flashlight_cookie_byte(m, bl.x1, bl.y1), bl.fx), bl.fy);
		store(m, spot_variable, fmul(m, 1, load(m, m->t_float, spot_variable), fselect(m, inside, cookie,
			constant_float(m, 0.0f))));
	}
	/* (the shadowed slot's map: a 3x3 tent, four bilinear comparisons) */
	if (shadowed)
	{
		unsigned int shadow_merge, sampled, image, mx, my, texel_width, bias, receiver, lit, facing_light;
		unsigned int lamp_v, lamp_z, lamp_inverse, lamp_inside;
		const float size = (float)XGPU_DYNAMIC_LIGHTS_FLASHLIGHT_SHADOW_SIZE;

		/* (the map's from the lamp: its own a, b and distance; a point
		behind the lamp, or outside the map, is not shadowed) */
		lamp_v = fsub(m, 3, p, flashlight_vector(m, constant_int(m, XGPU_DYNAMIC_LIGHTS_FLASHLIGHT + 23)));
		lamp_z = fdot(m, lamp_v, forward);
		lamp_inverse = fdiv(m, 1, constant_float(m, 1.0f), fmul(m, 1, fmax_scalar(m, 1, lamp_z, 1.0e-4f),
			fmax_scalar(m, 1, width, 1.0e-4f)));
		a = fmul(m, 1, fdot(m, lamp_v, left), lamp_inverse);
		b = fmul(m, 1, fdot(m, lamp_v, up), lamp_inverse);
		lamp_inside = op2(m, OP_LOGICAL_AND, m->t_bool, compare(m, OP_F_ORD_GREATER_THAN, lamp_z, constant_float(m, 1.0e-4f)),
			op2(m, OP_LOGICAL_AND, m->t_bool,
			compare(m, OP_F_ORD_LESS_THAN, ext1(m, GLSL_F_ABS, m->t_float, a), constant_float(m, 1.0f)),
			compare(m, OP_F_ORD_LESS_THAN, ext1(m, GLSL_F_ABS, m->t_float, b), constant_float(m, 1.0f))));
		selection_begin(m, op2(m, OP_LOGICAL_AND, m->t_bool, op2(m, OP_LOGICAL_AND, m->t_bool, inside, lamp_inside),
			op2(m, OP_I_EQUAL, m->t_bool, uextract(m, read->head, 0), op1(m, OP_BITCAST, m->t_uint, int_add(m, slot, 1)))),
			&shadow_merge);
		distance = flength(m, lamp_v);
		flashlight_shadow_declare(m);
		sampled = load(m, type_sampled_image(m, DIM_2D, FALSE, FALSE, TRUE), m->flashlight_shadow);
		image = op(m, OP_IMAGE, type_sampled_image(m, DIM_2D, FALSE, FALSE, FALSE), 1, &sampled);
		mx = fsub(m, 1, fmul(m, 1, fadd(m, 1, fmul(m, 1, a, constant_float(m, 0.5f)), constant_float(m, 0.5f)),
			constant_float(m, size)), constant_float(m, 0.5f));
		my = fsub(m, 1, fmul(m, 1, fadd(m, 1, fmul(m, 1, b, constant_float(m, 0.5f)), constant_float(m, 0.5f)),
			constant_float(m, size)), constant_float(m, 0.5f));
		/* bias = head.w + distance * (2 R / size) * 1.5 / max(|dot(surface_normal, l)|, 0.25): a texel
		and a half of the map at the receiver along its slope, so the caster does not shadow itself */
		texel_width = fmul(m, 1, distance, fmul(m, 1, width, constant_float(m, 2.0f * 1.5f / size)));
		facing_light = fmax_scalar(m, 1, ext1(m, GLSL_F_ABS, m->t_float, fdot(m, surface_normal, l)), 0.25f);
		bias = fadd(m, 1, op1(m, OP_BITCAST, m->t_float, uextract(m, read->head, 3)), fdiv(m, 1, texel_width,
			facing_light));
		receiver = fsub(m, 1, distance, bias);
		lit = fadd(m, 1, fadd(m, 1, flashlight_shadow_compare(m, image, fsub(m, 1, mx, constant_float(m, 0.5f)),
			fsub(m, 1, my, constant_float(m, 0.5f)), receiver), flashlight_shadow_compare(m, image,
			fadd(m, 1, mx, constant_float(m, 0.5f)), fsub(m, 1, my, constant_float(m, 0.5f)), receiver)),
			fadd(m, 1, flashlight_shadow_compare(m, image, fsub(m, 1, mx, constant_float(m, 0.5f)),
			fadd(m, 1, my, constant_float(m, 0.5f)), receiver), flashlight_shadow_compare(m, image,
			fadd(m, 1, mx, constant_float(m, 0.5f)), fadd(m, 1, my, constant_float(m, 0.5f)), receiver)));
		lit = fmul(m, 1, lit, constant_float(m, 0.25f));
		store(m, spot_variable, fmul(m, 1, load(m, m->t_float, spot_variable), lit));
		/* (HALO_FLASHLIGHT_SHADOW_VIEW=1 or 3, checking: the map's share seen,
		whole, in place of the gel and falloff; 2: the map's distance, a
		world unit a band) */
		if (getenv("HALO_FLASHLIGHT_SHADOW_VIEW") && (atoi(getenv("HALO_FLASHLIGHT_SHADOW_VIEW")) == 1 ||
			atoi(getenv("HALO_FLASHLIGHT_SHADOW_VIEW")) == 3))
		{
			store(m, spot_variable, lit);
			store(m, falloff_variable, constant_float(m, 1.0f));
		}
		if (getenv("HALO_FLASHLIGHT_SHADOW_VIEW") && atoi(getenv("HALO_FLASHLIGHT_SHADOW_VIEW")) == 2)
		{
			unsigned int operands[4];

			operands[0] = image;
			operands[1] = op(m, OP_COMPOSITE_CONSTRUCT, m->t_iv2, 2, (unsigned int[]){
				op1(m, OP_CONVERT_F_TO_S, m->t_int, fadd(m, 1, mx, constant_float(m, 0.5f))),
				op1(m, OP_CONVERT_F_TO_S, m->t_int, fadd(m, 1, my, constant_float(m, 0.5f))) });
			operands[2] = IMAGE_OPERANDS_LOD;
			operands[3] = constant_int(m, 0);
			store(m, spot_variable, ext1(m, GLSL_FRACT, m->t_float, extract(m, op(m, OP_IMAGE_FETCH, m->t_v4, 4, operands), 0)));
			store(m, falloff_variable, constant_float(m, 1.0f));
		}
		selection_end(m, shadow_merge);
	}
	selection_end(m, merge);
}

/* dynamic_light_sum(p, n, surface_normal, environment): the lights of the
pixel's cell, each as the light model has it (xgpu.h: as the game adds it
to the structure or to a model, in its gamma), summed, the panels' linear
light summed into the linear variable apart (a model's but those its object's own that do not light
it, its model_lights[XGPU_MODEL_LIGHT_OWN_LIGHTS]); its loops are SPIR-V's structured loops, its values
kept in the function's variables. With a reflection (a model's, of the eye
about its normal; 0 none), each light's highlight along it, to the exponent,
summed into the shine variable as well (xgpu.h XGPU_DYNAMIC_LIGHTS_HIGHLIGHT;
xgpu_msl.c's dynamic_light_sum) */
static unsigned int dynamic_light_sum(struct module *m, struct pixel_state *state, unsigned int p, unsigned int n,
	unsigned int surface_normal, BOOL environment, unsigned int reflection, unsigned int exponent,
	unsigned int shine_variable, unsigned int linear_variable)
{
	unsigned int grid0 = dynamic_light(m, 0, constant_int(m, 0)), grid1 = dynamic_light(m, 0, constant_int(m, 1));
	unsigned int offset, cell, mask, cell_index, operands[4];
	unsigned int sum_variable, word_variable, bits_variable;
	unsigned int outer_header, outer_condition, outer_body, outer_continue, outer_merge;
	unsigned int inner_header, inner_condition, inner_body, inner_continue, inner_merge;
	unsigned int word, bits, light, position, to_light, distance_squared, x2, l, falloff, facing;
	unsigned int cone, color, spot, w, own = 0, tint = 0, panels, panel, panel_falloff, a2;
	unsigned int light_falloff, light_spot, lit_light;
	struct light_shadows_read shadows;
	struct flashlight_read flashlight;

	/* cell = sign(offset) * log2(1.0 + abs(offset) * grid[1].w) * grid[1].xyz + vec3(16.0, 16.0, 8.0) */
	offset = fsub(m, 3, p, xyz(m, grid0));
	cell = fadd(m, 3, fmul(m, 3, fmul(m, 3, ext1(m, GLSL_F_SIGN, m->t_v3, offset),
		ext1(m, GLSL_LOG2, m->t_v3, fadd(m, 3, constant_splat(m, 3, 1.0f),
		fscale(m, 3, ext1(m, GLSL_F_ABS, m->t_v3, offset), extract(m, grid1, 3))))), xyz(m, grid1)),
		constant_vector(m, 3, (const float[]){ (float)(XGPU_DYNAMIC_LIGHTS_GRID_X / 2), (float)(XGPU_DYNAMIC_LIGHTS_GRID_Y / 2),
		(float)(XGPU_DYNAMIC_LIGHTS_GRID_Z / 2) }));
	cell_index = ext3(m, GLSL_S_CLAMP, m->t_iv3, op1(m, OP_CONVERT_F_TO_S, m->t_iv3, ext1(m, GLSL_FLOOR, m->t_v3, cell)),
		op(m, OP_COMPOSITE_CONSTRUCT, m->t_iv3, 3, (unsigned int[]){ constant_int(m, 0), constant_int(m, 0), constant_int(m, 0) }),
		op(m, OP_COMPOSITE_CONSTRUCT, m->t_iv3, 3, (unsigned int[]){ constant_int(m, XGPU_DYNAMIC_LIGHTS_GRID_X - 1),
		constant_int(m, XGPU_DYNAMIC_LIGHTS_GRID_Y - 1), constant_int(m, XGPU_DYNAMIC_LIGHTS_GRID_Z - 1) }));
	mask = dynamic_light_texel(m, cell_index);
	/* (those of the cell's lights that reach the pixel's cluster, its point
	a little off its surface: xgpu.h) */
	mask = dynamic_light_cluster_mask(m, state, mask, fadd(m, 3, p, fscale(m, 3, surface_normal,
		constant_float(m, XGPU_DYNAMIC_LIGHTS_CLUSTER_OFFSET))), environment);
	/* (a model's: the lights it is not lit by, xgpu.h) */
	if (!environment)
		own = uniform(m, U_MODEL_LIGHTS, XGPU_MODEL_LIGHT_OWN_LIGHTS);
	light_shadows_begin(m, state, &shadows);
	flashlight_begin(m, &flashlight);
	/* (the panels among the lights, a bit each: xgpu.h's XGPU_DYNAMIC_LIGHTS_PANELS) */
	panels = flashlight_texel(m, constant_int(m, XGPU_DYNAMIC_LIGHTS_PANELS));
	light_falloff = local(m, &state->locals, m->t_float);
	light_spot = local(m, &state->locals, m->t_float);
	store(m, linear_variable, constant_splat(m, 3, 0.0f));

	sum_variable = local(m, &state->locals, m->t_v3);
	word_variable = local(m, &state->locals, m->t_int);
	bits_variable = local(m, &state->locals, m->t_uint);
	store(m, sum_variable, constant_splat(m, 3, 0.0f));
	store(m, word_variable, constant_int(m, 0));

	outer_header = id_new(m);
	outer_condition = id_new(m);
	outer_body = id_new(m);
	outer_continue = id_new(m);
	outer_merge = id_new(m);
	branch(m, outer_header);
	label_at(m, outer_header);
	operands[0] = outer_merge;
	operands[1] = outer_continue;
	operands[2] = 0;
	op_void(m, OP_LOOP_MERGE, 3, operands);
	branch(m, outer_condition);
	label_at(m, outer_condition);
	word = load(m, m->t_int, word_variable);
	branch_conditional(m, op2(m, OP_S_LESS_THAN, m->t_bool, word, constant_int(m, 4)), outer_body, outer_merge);
	label_at(m, outer_body);
	store(m, bits_variable, op2(m, OP_VECTOR_EXTRACT_DYNAMIC, m->t_uint, mask, word));

	inner_header = id_new(m);
	inner_condition = id_new(m);
	inner_body = id_new(m);
	inner_continue = id_new(m);
	inner_merge = id_new(m);
	branch(m, inner_header);
	label_at(m, inner_header);
	operands[0] = inner_merge;
	operands[1] = inner_continue;
	operands[2] = 0;
	op_void(m, OP_LOOP_MERGE, 3, operands);
	branch(m, inner_condition);
	label_at(m, inner_condition);
	bits = load(m, m->t_uint, bits_variable);
	branch_conditional(m, op2(m, OP_I_NOT_EQUAL, m->t_bool, bits, constant_uint(m, 0)), inner_body, inner_merge);
	label_at(m, inner_body);
	/* light = word * 32 + findLSB(bits); bits &= bits - 1u */
	light = op2(m, OP_I_ADD, m->t_int, op2(m, OP_I_MUL, m->t_int, word, constant_int(m, 32)),
		ext1(m, GLSL_FIND_I_LSB, m->t_int, bits));
	store(m, bits_variable, op2(m, OP_BITWISE_AND, m->t_uint, bits, op2(m, OP_I_SUB, m->t_uint, bits, constant_uint(m, 1))));
	position = dynamic_light(m, 1, light);
	w = extract(m, position, 3);
	/* (HALO_LT_E1: every light that lights the structure (position.w > 0;
	not a models-only light, whose structure the game's own gel pass lights)
	is the traced pass's, added after the sum, lt_e1_add: if (position.w >
	0.0) continue;) */
	if (lt_e1_on() && state->rt_bounce)
	{
		unsigned int skip = id_new(m), condition = compare(m, OP_F_ORD_GREATER_THAN, w, constant_float(m, 0.0f));

		selection_merge(m, skip);
		branch_conditional(m, condition, inner_continue, skip);
		label_at(m, skip);
	}
	/* (a forward receiver of the one light model: the lights the pass
	lights, which it adds itself by the pass's model, lt_forward_add: if
	(lt_forward_skips(light)) continue;) */
	if (state->lt_forward)
	{
		unsigned int skip = id_new(m), condition = lt_forward_skips(m, state, light);

		selection_merge(m, skip);
		branch_conditional(m, condition, inner_continue, skip);
		label_at(m, skip);
	}
	if (environment)
	{
		unsigned int skip = id_new(m), condition = compare(m, OP_F_ORD_LESS_THAN, w, constant_float(m, 0.0f));

		/* if (environment && position.w < 0.0) continue; */
		selection_merge(m, skip);
		branch_conditional(m, condition, inner_continue, skip);
		label_at(m, skip);
	}
	else
	{
		unsigned int skip = id_new(m), index = op1(m, OP_CONVERT_S_TO_F, m->t_float, light), condition;
		int slot;

		/* if (any(own == vec4(float(light)))) continue; */
		condition = compare(m, OP_F_ORD_EQUAL, extract(m, own, 0), index);
		for (slot = 1; slot < 4; slot++)
			condition = op2(m, OP_LOGICAL_OR, m->t_bool, condition, compare(m, OP_F_ORD_EQUAL, extract(m, own, slot), index));
		selection_merge(m, skip);
		branch_conditional(m, condition, inner_continue, skip);
		label_at(m, skip);
	}
	to_light = fsub(m, 3, xyz(m, position), p);
	distance_squared = fdot(m, to_light, to_light);
	x2 = fmul(m, 1, fmul(m, 1, distance_squared, w), w);
	{
		unsigned int skip = id_new(m), condition = compare(m, OP_F_ORD_GREATER_THAN_EQUAL, x2, constant_float(m, 1.0f));

		/* if (x2 >= 1.0) continue; */
		selection_merge(m, skip);
		branch_conditional(m, condition, inner_continue, skip);
		label_at(m, skip);
	}
	l = fscale(m, 3, to_light, ext1(m, GLSL_INVERSE_SQRT, m->t_float, fmax_scalar(m, 1, distance_squared, (float)1.0e-8)));
	cone = dynamic_light(m, 3, light);
	color = dynamic_light(m, 2, light);
	/* (the light model, xgpu.h: as the game adds the light to the surface) */
	if (environment)
	{
		/* the gel pass: T(d / r) * max(dot(n, l), 0) * saturate(8 dot(surface_normal, l)), its gel (1 a point's) */
		falloff = light_attenuation(m, fsqrt(m, distance_squared), w);
		facing = fmul(m, 1, fmax_scalar(m, 1, fdot(m, n, l), 0.0f),
			saturate(m, 1, fmul(m, 1, fdot(m, surface_normal, l), constant_float(m, 8.0f))));
		spot = constant_float(m, 1.0f);
	}
	else
	{
		/* the model lighting: max(1 - d^2 / r^2, 0) * saturate(dot(n, l)) * saturate(dot(l, -axis) * scale + offset) */
		falloff = fsub(m, 1, constant_float(m, 1.0f), x2);
		facing = saturate(m, 1, fdot(m, n, l));
		spot = saturate(m, 1, fadd(m, 1, fmul(m, 1, fdot(m, l, fneg(m, 3, xyz(m, cone))), extract(m, cone, 3)),
			extract(m, color, 3)));
	}
	/* (a panel: a^2 / (a^2 + d^2) * (1 - d^2 / r^2)^2 by the cosine off its face, linear) */
	panel = op1(m, OP_CONVERT_U_TO_F, m->t_float, op2(m, OP_BITWISE_AND, m->t_uint, op2(m, OP_SHIFT_RIGHT_LOGICAL, m->t_uint,
		op2(m, OP_VECTOR_EXTRACT_DYNAMIC, m->t_uint, panels, word), op1(m, OP_BITCAST, m->t_uint,
		op2(m, OP_BITWISE_AND, m->t_int, light, constant_int(m, 31)))), constant_uint(m, 1)));
	a2 = fmax_scalar(m, 1, extract(m, color, 3), 1.0e-8f);
	panel_falloff = fmul(m, 1, fdiv(m, 1, a2, fadd(m, 1, a2, distance_squared)),
		fmul(m, 1, fsub(m, 1, constant_float(m, 1.0f), x2), fsub(m, 1, constant_float(m, 1.0f), x2)));
	{
		unsigned int is_panel = compare(m, OP_F_ORD_GREATER_THAN, panel, constant_float(m, 0.5f));

		falloff = fselect(m, is_panel, panel_falloff, falloff);
		spot = fselect(m, is_panel, saturate(m, 1, fdot(m, l, fneg(m, 3, xyz(m, cone)))), spot);
	}
	falloff = light_shadows_factor(m, &shadows, light, falloff, &tint);
	/* (HALO_LIGHT_SHADOWS_VIEW: a shadowed light in its tint) */
	if (tint)
		color = vec4_of(m, select_n(m, 3, compare(m, OP_F_ORD_GREATER_THAN, extract(m, tint, 3), constant_float(m, 0.5f)),
			xyz(m, tint), xyz(m, color)), extract(m, color, 3));
	/* (the flashlight's: its gel on the structure, its shadow map everywhere) */
	store(m, light_falloff, falloff);
	store(m, light_spot, spot);
	flashlight_light(m, &flashlight, light, p, position, l, surface_normal, environment, TRUE, light_falloff, light_spot);
	falloff = load(m, m->t_float, light_falloff);
	spot = load(m, m->t_float, light_spot);
	/* (the game's lights in its gamma, summed; the panels' linear apart) */
	lit_light = fscale(m, 3, xyz(m, color), fmul(m, 1, fmul(m, 1, falloff, facing), spot));
	store(m, sum_variable, fadd(m, 3, load(m, m->t_v3, sum_variable), fscale(m, 3, lit_light,
		fsub(m, 1, constant_float(m, 1.0f), panel))));
	store(m, linear_variable, fadd(m, 3, load(m, m->t_v3, linear_variable), fscale(m, 3, lit_light, panel)));
	if (reflection)
	{
		/* shine += color.rgb^2 * (falloff * spot * saturate(dot(n, l) * 4.0) * pow(max(dot(reflection, l), 0.0), exponent))
		(linear: the colour's square, about its gamma's) */
		unsigned int lobe = ext2(m, GLSL_POW, m->t_float, fmax_scalar(m, 1, fdot(m, reflection, l), 0.0f), exponent);
		unsigned int horizon = saturate(m, 1, fmul(m, 1, fdot(m, n, l), constant_float(m, 4.0f)));

		store(m, shine_variable, fadd(m, 3, load(m, m->t_v3, shine_variable),
			fscale(m, 3, fmul(m, 3, xyz(m, color), xyz(m, color)), fmul(m, 1, fmul(m, 1, fmul(m, 1, falloff, spot), horizon),
			lobe))));
	}
	branch(m, inner_continue);
	label_at(m, inner_continue);
	branch(m, inner_header);
	label_at(m, inner_merge);
	branch(m, outer_continue);
	label_at(m, outer_continue);
	store(m, word_variable, op2(m, OP_I_ADD, m->t_int, load(m, m->t_int, word_variable), constant_int(m, 1)));
	branch(m, outer_header);
	label_at(m, outer_merge);
	return load(m, m->t_v3, sum_variable);
}

/* dynamic_light_add(base, light, linear): the light model's sum (xgpu.h)
added to the light already there as the game's framebuffer adds it, the
base and the sum in its gamma, (base + light)^2.2, and the panels' linear
light (0 none) added there, under a shoulder that leaves all below
max(base^2.2, ADD_KNEE) as it is (so a light that leaves the pixel below
white is the game's own) and rolls the rest off toward white; with
display.hdr past white too, toward the lights' white (xgpu_hdr.h's
xgpu_hdr_lights). The base as it is (saturated) where there is no light */
static unsigned int dynamic_light_add(struct module *m, struct pixel_state *state, unsigned int base,
	unsigned int light, unsigned int linear, BOOL hdr)
{
	unsigned int had_gamma, had, knee, room, y, soft, gamma, added, after, any_light, result_variable;

	result_variable = local(m, &state->locals, m->t_v3);
	store(m, result_variable, saturate(m, 3, base));
	any_light = op1(m, OP_ANY, m->t_bool, op2(m, OP_F_UNORD_NOT_EQUAL, m->t_bv3, light, constant_splat(m, 3, 0.0f)));
	if (linear)
		any_light = op2(m, OP_LOGICAL_OR, m->t_bool, any_light, op1(m, OP_ANY, m->t_bool, op2(m, OP_F_UNORD_NOT_EQUAL,
			m->t_bv3, linear, constant_splat(m, 3, 0.0f))));
	added = id_new(m);
	after = id_new(m);
	selection_merge(m, after);
	branch_conditional(m, any_light, added, after);
	label_at(m, added);
	gamma = constant_splat(m, 3, 2.2f);
	had_gamma = saturate(m, 3, base);
	had = ext2(m, GLSL_POW, m->t_v3, had_gamma, gamma);
	y = ext2(m, GLSL_POW, m->t_v3, fadd(m, 3, had_gamma, fmax_scalar(m, 3, light, 0.0f)), gamma);
	if (linear)
		y = fadd(m, 3, y, fmax_scalar(m, 3, linear, 0.0f));
	/* soft = y <= k ? y : k + (1 - k) (1 - exp(-(y - k) / (1 - k))), k = max(had, ADD_KNEE) */
	knee = fmax_scalar(m, 3, had, XGPU_DYNAMIC_LIGHTS_ADD_KNEE);
	room = fmax_scalar(m, 3, fsub(m, 3, constant_splat(m, 3, 1.0f), knee), (float)1.0e-3);
	soft = fadd(m, 3, knee, fmul(m, 3, room, fsub(m, 3, constant_splat(m, 3, 1.0f), ext1(m, GLSL_EXP, m->t_v3,
		fdiv(m, 3, fneg(m, 3, fmax_scalar(m, 3, fsub(m, 3, y, knee), 0.0f)), room)))));
	soft = op3(m, OP_SELECT, m->t_v3, op2(m, OP_F_ORD_LESS_THAN_EQUAL, m->t_bv3, y, knee), y, soft);
	if (hdr)
	{
		unsigned int past = fmax_scalar(m, 3, fsub(m, 3, y, constant_splat(m, 3, 1.0f)), 0.0f);
		unsigned int white_room = constant_splat(m, 3, XGPU_HDR_LIGHTS_WHITE - 1.0f);

		soft = fadd(m, 3, soft, fmul(m, 3, white_room, fsub(m, 3, constant_splat(m, 3, 1.0f),
			ext1(m, GLSL_EXP, m->t_v3, fdiv(m, 3, fneg(m, 3, past), white_room)))));
	}
	store(m, result_variable, ext2(m, GLSL_POW, m->t_v3, soft, constant_splat(m, 3, (float)(1.0 / 2.2))));
	branch(m, after);
	label_at(m, after);
	return load(m, m->t_v3, result_variable);
}

/* ---------- display.hdr's tags (xgpu_hdr.h; xgpu_msl.c's hdr_output, operation
for operation)

A draw whose tag blends in the shader (XGPU_HDR_SHADER_BLENDS) reads the
pixel it draws over as input attachment 0 (XGPU_SPIRV_SET_INPUT, binding 0:
the colour attachment, read where it is drawn, VK_KHR_dynamic_rendering_
local_read's), each sample's own where the target is multisampled, and
blends with it here; the pipeline's blending is off. */

static unsigned int brightest_of(struct module *m, unsigned int rgb)
{
	return ext2(m, GLSL_F_MAX, m->t_float, extract(m, rgb, 0), ext2(m, GLSL_F_MAX, m->t_float, extract(m, rgb, 1),
		extract(m, rgb, 2)));
}

/* display.hdr_highlight_boost (#1123): what a boost's level adds to rgb,
the light it has past what was under it (added, at least 0) gained, its
brightest no further than the ceiling, nor than itself where it is
already past: xgpu_hdr_boost_toward */
static unsigned int boost_extra(struct module *m, unsigned int rgb, unsigned int added, int level, float ceiling)
{
	unsigned int extra = fscale(m, 3, added, constant_float(m, XGPU_HDR_BOOST_STEP * (float)level));
	unsigned int share = fclamp(m, 1, fdiv(m, 1, fsub(m, 1, constant_float(m, ceiling), brightest_of(m, rgb)),
		fmax_scalar(m, 1, brightest_of(m, extra), 1.0e-6f)), 0.0f, 1.0f);

	return fscale(m, 3, extra, share);
}

/* where an interlocked draw writes its colour (hdr_destination): its
target as a storage image, the pixel's place and the sample's (0: none) */
struct hdr_store
{
	unsigned int image, place, sample;
};

/* the pixel drawn over: as an input attachment, or, where the draw is
interlocked (options->interlock), from its target as a storage image at
XGPU_SPIRV_BINDING_STORE, whose write (store) ends the critical section the
read begins: a colour attachment's write lands after the shader, outside
it, where the next triangle's read may not see it */
static unsigned int hdr_destination(struct module *m, const struct xgpu_spirv_options *options,
	unsigned int *frag_coord, struct hdr_store *store)
{
	BOOL multisampled = options->input_samples > 1;
	unsigned int operands[9], image_type, image, capability;
	int count = 2;

	operands[0] = m->t_float;
	operands[1] = options->interlock ? DIM_2D : DIM_SUBPASS_DATA;
	operands[2] = 0;
	operands[3] = 0;
	operands[4] = multisampled ? 1 : 0;
	operands[5] = 2;
	operands[6] = options->interlock ? IMAGE_FORMAT_RGBA16F : 0;
	image_type = intern(m, OP_TYPE_IMAGE, FALSE, 7, operands);
	image = variable(m, image_type, STORAGE_UNIFORM_CONSTANT, "xgpu_destination");
	decorate1(m, image, DECORATION_DESCRIPTOR_SET, XGPU_SPIRV_SET_INPUT);
	if (options->interlock)
	{
		decorate1(m, image, DECORATION_BINDING, XGPU_SPIRV_BINDING_STORE);
		decorate(m, image, DECORATION_COHERENT, 0, NULL);
		if (multisampled)
		{
			capability = CAPABILITY_STORAGE_IMAGE_MULTISAMPLE;
			instruction(m, SECTION_CAPABILITIES, OP_CAPABILITY, 1, &capability);
		}
		if (!*frag_coord)
		{
			*frag_coord = variable(m, m->t_v4, STORAGE_INPUT, "gl_FragCoord");
			decorate1(m, *frag_coord, DECORATION_BUILT_IN, BUILT_IN_FRAG_COORD);
		}
		operands[1] = op1(m, OP_CONVERT_F_TO_S, m->t_iv2, xy(m, load(m, m->t_v4, *frag_coord)));
	}
	else
	{
		capability = CAPABILITY_INPUT_ATTACHMENT;
		instruction(m, SECTION_CAPABILITIES, OP_CAPABILITY, 1, &capability);
		decorate1(m, image, DECORATION_BINDING, 0);
		decorate1(m, image, DECORATION_INPUT_ATTACHMENT_INDEX, 0);
		operands[1] = constant_int(m, 0);
		operands[1] = intern(m, OP_CONSTANT_COMPOSITE, TRUE, 3, (const unsigned int[]){ m->t_iv2, operands[1],
			operands[1] });
	}
	operands[0] = load(m, image_type, image);
	store->image = operands[0];
	store->place = operands[1];
	store->sample = 0;
	if (multisampled)
	{
		unsigned int sample = variable(m, m->t_int, STORAGE_INPUT, "gl_SampleID");

		capability = CAPABILITY_SAMPLE_RATE_SHADING;
		instruction(m, SECTION_CAPABILITIES, OP_CAPABILITY, 1, &capability);
		decorate1(m, sample, DECORATION_BUILT_IN, BUILT_IN_SAMPLE_ID);
		decorate(m, sample, DECORATION_FLAT, 0, NULL);
		operands[count++] = IMAGE_OPERANDS_SAMPLE;
		operands[count++] = store->sample = load(m, m->t_int, sample);
	}
	/* (interlocked: the pixel's, or each sample's own, read and written by
	one invocation at a time, in the order the triangles were drawn, from
	here to the store) */
	if (options->interlock)
		op_void(m, OP_BEGIN_INVOCATION_INTERLOCK, 0, NULL);
	return op(m, OP_IMAGE_READ, m->t_v4, count, operands);
}

/* an interlocked draw's colour written to its target (hdr_destination),
the channels its mask leaves as they were, and the critical section ended */
static void hdr_store(struct module *m, const struct xgpu_spirv_options *options, const struct hdr_store *store,
	unsigned int destination, unsigned int color)
{
	unsigned int operands[5], components[4];
	int count = 3, channel;

	if ((options->interlock_mask & 15) != 15)
	{
		for (channel = 0; channel < 4; channel++)
			components[channel] = extract(m, (options->interlock_mask & (1 << channel)) ? color : destination,
				channel);
		color = op(m, OP_COMPOSITE_CONSTRUCT, m->t_v4, 4, components);
	}
	operands[0] = store->image;
	operands[1] = store->place;
	operands[2] = color;
	if (store->sample)
	{
		operands[count++] = IMAGE_OPERANDS_SAMPLE;
		operands[count++] = store->sample;
	}
	op_void(m, OP_IMAGE_WRITE, count, operands);
	op_void(m, OP_END_INVOCATION_INTERLOCK, 0, NULL);
}

/* a blend factor for every channel at once, of the source (s), the pixel
drawn over (d: past white, one less it is never below 0) and the blend
colour (a colour's factor acts on alpha as its alpha's) */
static unsigned int hdr_factor(struct module *m, int factor, unsigned int s, unsigned int d)
{
	unsigned int one = constant_splat(m, 4, 1.0f), one1 = constant_float(m, 1.0f);

	switch (factor)
	{
	case XGPU_BLEND_ZERO: return constant_splat(m, 4, 0.0f);
	case XGPU_BLEND_ONE: return one;
	case XGPU_BLEND_SOURCE_COLOR: return s;
	case XGPU_BLEND_ONE_MINUS_SOURCE_COLOR: return fsub(m, 4, one, s);
	case XGPU_BLEND_SOURCE_ALPHA: return splat(m, 4, extract(m, s, 3));
	case XGPU_BLEND_ONE_MINUS_SOURCE_ALPHA: return splat(m, 4, fsub(m, 1, one1, extract(m, s, 3)));
	case XGPU_BLEND_DESTINATION_ALPHA: return splat(m, 4, extract(m, d, 3));
	case XGPU_BLEND_ONE_MINUS_DESTINATION_ALPHA:
		return splat(m, 4, fmax_scalar(m, 1, fsub(m, 1, one1, extract(m, d, 3)), 0.0f));
	case XGPU_BLEND_DESTINATION_COLOR: return d;
	case XGPU_BLEND_ONE_MINUS_DESTINATION_COLOR: return fmax_scalar(m, 4, fsub(m, 4, one, d), 0.0f);
	case XGPU_BLEND_SOURCE_ALPHA_SATURATE:
		return vec4_of(m, splat(m, 3, ext2(m, GLSL_F_MIN, m->t_float, extract(m, s, 3),
			fmax_scalar(m, 1, fsub(m, 1, one1, extract(m, d, 3)), 0.0f))), one1);
	case XGPU_BLEND_CONSTANT_COLOR: return uniform(m, U_BLEND_CONSTANT, -1);
	case XGPU_BLEND_ONE_MINUS_CONSTANT_COLOR: return fsub(m, 4, one, uniform(m, U_BLEND_CONSTANT, -1));
	case XGPU_BLEND_CONSTANT_ALPHA: return splat(m, 4, extract(m, uniform(m, U_BLEND_CONSTANT, -1), 3));
	case XGPU_BLEND_ONE_MINUS_CONSTANT_ALPHA:
		return splat(m, 4, fsub(m, 1, one1, extract(m, uniform(m, U_BLEND_CONSTANT, -1), 3)));
	default: return one;
	}
}

/* the colour written for a tagged draw, from its source (s: the pixel
shader's result as the game's blend takes it): blended here with the pixel
drawn over and the key's blend, a glow brightened toward its ceiling, no
channel taken past what the pixel had or the ceiling, nor below 0,
destination alpha not past 1 */
static unsigned int hdr_output(struct module *m, unsigned short hdr, unsigned int s,
	const struct xgpu_spirv_options *options, unsigned int *frag_coord, struct hdr_store *store, unsigned int *read)
{
	int ceiling = XGPU_HDR_CEILING_OF(hdr);
	float ceiling_value = ceiling == XGPU_HDR_CEILING_WEAPON ? XGPU_HDR_CEILING_WEAPON_VALUE :
		ceiling == XGPU_HDR_CEILING_HUD ? XGPU_HDR_CEILING_HUD_VALUE :
		ceiling == XGPU_HDR_CEILING_LIGHT ? XGPU_HDR_CEILING_LIGHT_VALUE :
		ceiling == XGPU_HDR_CEILING_NUMERIC ? XGPU_HDR_CEILING_NUMERIC_VALUE : XGPU_HDR_CEILING_PAPER_WHITE_VALUE;
	float knee = ceiling == XGPU_HDR_CEILING_LIGHT || ceiling == XGPU_HDR_CEILING_NUMERIC ? XGPU_HDR_EXPAND_LIGHT_KNEE :
		XGPU_HDR_EXPAND_KNEE;
	unsigned int d = *read = hdr_destination(m, options, frag_coord, store), v, rgb, had, alpha;
	unsigned int source = fmul(m, 4, s, hdr_factor(m, XGPU_HDR_BLEND_SOURCE(hdr), s, d));
	unsigned int destination = fmul(m, 4, d, hdr_factor(m, XGPU_HDR_BLEND_DESTINATION(hdr), s, d));

	switch (XGPU_HDR_BLEND_OPERATION(hdr))
	{
	case XGPU_BLEND_SUBTRACT: v = fsub(m, 4, source, destination); break;
	case XGPU_BLEND_REVERSE_SUBTRACT: v = fsub(m, 4, destination, source); break;
	case XGPU_BLEND_MIN: v = ext2(m, GLSL_F_MIN, m->t_v4, s, d); break;
	case XGPU_BLEND_MAX: v = ext2(m, GLSL_F_MAX, m->t_v4, s, d); break;
	default: v = fadd(m, 4, source, destination); break;
	}
	v = fmax_scalar(m, 4, v, 0.0f);
	rgb = xyz(m, v);
	had = xyz(m, d);
	/* (a glow: more of the light the draw added, where it is bright:
	xgpu_hdr_expand) */
	if (XGPU_HDR_GLOW(hdr))
	{
		unsigned int added = fmax_scalar(m, 3, fsub(m, 3, rgb, had), 0.0f);
		unsigned int added_most = brightest_of(m, added), most = brightest_of(m, rgb), more;
		unsigned int step = ext3(m, GLSL_SMOOTH_STEP, m->t_float, constant_float(m, knee),
			constant_float(m, XGPU_HDR_EXPAND_TO_CEILING(ceiling) ? XGPU_HDR_EXPAND_NUMERIC_FULL : 1.0f), most);

		/* (the reach: the ceiling's share of the pixel, or for the
		weapon's numbers what is left under the ceiling) */
		more = XGPU_HDR_EXPAND_TO_CEILING(ceiling) ?
			fmul(m, 1, fsub(m, 1, constant_float(m, ceiling_value), most), step) :
			fmul(m, 1, fmul(m, 1, constant_float(m, ceiling_value - 1.0f), step), most);
		more = ext2(m, GLSL_F_MIN, m->t_float, fdiv(m, 1, more, fmax_scalar(m, 1, added_most, 1.0e-6f)),
			constant_float(m, XGPU_HDR_EXPAND_MOST));
		more = op3(m, OP_SELECT, m->t_float, compare(m, OP_F_ORD_GREATER_THAN, added_most, constant_float(m, 0.0f)),
			more, constant_float(m, 0.0f));
		rgb = fadd(m, 3, rgb, fscale(m, 3, added, more));
	}
	/* (a glow's ceiling keeps its hue: its brightest channel's; any other's
	each channel's) */
	if (XGPU_HDR_GLOW(hdr))
	{
		unsigned int limit = fmax_scalar(m, 1, brightest_of(m, had), ceiling_value), brightest = brightest_of(m, rgb);

		rgb = fscale(m, 3, rgb, op3(m, OP_SELECT, m->t_float, compare(m, OP_F_ORD_GREATER_THAN, brightest, limit),
			fdiv(m, 1, limit, fmax_scalar(m, 1, brightest, 1.0e-6f)), constant_float(m, 1.0f)));
	}
	else if (ceiling)
		rgb = ext2(m, GLSL_F_MIN, m->t_v3, rgb, fmax_scalar(m, 3, had, ceiling_value));
	alpha = extract(m, v, 3);
	if (hdr & XGPU_HDR_ALPHA_LIMIT)
		alpha = ext2(m, GLSL_F_MIN, m->t_float, alpha, constant_float(m, 1.0f));
	return vec4_of(m, rgb, alpha);
}

/* ---------- the whole pixel shader */

/* the combiners and the final combiner: the pixel's colour (state->
zeroed_stages: the stages' constants that read as 0) */
static unsigned int combiners(struct module *m, struct pixel_state *state)
{
	const struct xgpu_final_combiner *final = &state->program->final;
	unsigned int ef_product, v1r0_sum, fa, fb, fc, fd, fg;
	int stage;

	for (stage = 0; stage < (int)state->program->combiner_count && stage < 8; stage++)
		combiner_stage(m, state, stage);
	if (!final->present)
		return state->r0;
	ef_product = vec4_of(m, fmul(m, 3, final_input(m, state, &final->inputs[4], FALSE, 0, 0),
		final_input(m, state, &final->inputs[5], FALSE, 0, 0)), constant_float(m, 0.0f));
	v1r0_sum = fadd(m, 3, final->invert_v1 ? fsub(m, 3, constant_splat(m, 3, 1.0f), saturate(m, 3, xyz(m, state->v1))) :
		saturate(m, 3, xyz(m, state->v1)), final->invert_r0 ? fsub(m, 3, constant_splat(m, 3, 1.0f),
		saturate(m, 3, xyz(m, state->r0))) : saturate(m, 3, xyz(m, state->r0)));
	v1r0_sum = vec4_of(m, v1r0_sum, constant_float(m, 0.0f));
	if (final->clamp_sum)
		v1r0_sum = saturate(m, 4, v1r0_sum);
	fa = final_input(m, state, &final->inputs[0], FALSE, v1r0_sum, ef_product);
	fb = final_input(m, state, &final->inputs[1], FALSE, v1r0_sum, ef_product);
	fc = final_input(m, state, &final->inputs[2], FALSE, v1r0_sum, ef_product);
	fd = final_input(m, state, &final->inputs[3], FALSE, v1r0_sum, ef_product);
	fg = final_input(m, state, &final->inputs[6], TRUE, v1r0_sum, ef_product);
	/* vec4(fA * fB + (1.0 - fA) * fC + fD, fG) */
	return vec4_of(m, fadd(m, 3, fadd(m, 3, fmul(m, 3, fa, fb),
		fmul(m, 3, fsub(m, 3, constant_splat(m, 3, 1.0f), fa), fc)), fd), fg);
}

static unsigned int comparison_opcode(unsigned long function)
{
	switch (function)
	{
	case D3DCMP_LESS: return OP_F_ORD_LESS_THAN;
	case D3DCMP_EQUAL: return OP_F_ORD_EQUAL;
	case D3DCMP_LESSEQUAL: return OP_F_ORD_LESS_THAN_EQUAL;
	case D3DCMP_GREATER: return OP_F_ORD_GREATER_THAN;
	case D3DCMP_NOTEQUAL: return OP_F_UNORD_NOT_EQUAL;
	case D3DCMP_GREATEREQUAL: return OP_F_ORD_GREATER_THAN_EQUAL;
	default: return 0;
	}
}

/* a stage that samples its texture */
static BOOL stage_samples(unsigned char mode)
{
	switch (mode)
	{
	case XGPU_TEXTURE_PROJECT2D:
	case XGPU_TEXTURE_PROJECT3D:
	case XGPU_TEXTURE_CUBEMAP:
	case XGPU_TEXTURE_BUMPENVMAP:
	case XGPU_TEXTURE_BUMPENVMAP_LUMINANCE:
	case XGPU_TEXTURE_DOT_ST:
	case XGPU_TEXTURE_DOT_REFLECT_DIFFUSE:
	case XGPU_TEXTURE_DOT_REFLECT_SPECULAR:
	case XGPU_TEXTURE_DOT_REFLECT_SPECULAR_CONSTANT:
	case XGPU_TEXTURE_DOT_STR_3D:
	case XGPU_TEXTURE_DOT_STR_CUBE:
	case XGPU_TEXTURE_DEPENDENT_AR:
	case XGPU_TEXTURE_DEPENDENT_GB:
		return TRUE;
	default:
		return FALSE;
	}
}

/* ---------- the VR mode's effects (xgpu_spirv.h) */

/* the thickness map's unit (port/linux/vulkan/xgpu_vulkan_effects.h's
XGPU_VULKAN_THICKNESS_UNIT: texels a stored 1 is) */
#define THICKNESS_UNIT 128.0f

BOOL xgpu_spirv_effect_possible(const struct xgpu_pixel_program *program)
{
	return (program->multiview == XGPU_MULTIVIEW_EYES || program->multiview == XGPU_MULTIVIEW_NONE) &&
		program->stages[0].mode == XGPU_TEXTURE_PROJECT2D &&
		program->stages[0].sampler == _xgpu_sampler_2d && !stage_samples(program->stages[2].mode) &&
		!stage_samples(program->stages[3].mode);
}

/* the effect's depth is the views' layers (an array), else on a flat
screen (no views: display.soft_particles) one 2D image */
static BOOL effect_depth_arrayed(const struct pixel_state *state)
{
	return state->options->multiview != XGPU_MULTIVIEW_NONE;
}

/* the effect's textures: the scene's depth (the views' layers) and the
sprite's thickness */
static void effect_declare(struct module *m, struct pixel_state *state)
{
	state->frag_coord = variable(m, m->t_v4, STORAGE_INPUT, "gl_FragCoord");
	decorate1(m, state->frag_coord, DECORATION_BUILT_IN, BUILT_IN_FRAG_COORD);
	state->effect_depth = variable(m, type_sampled_image(m, DIM_2D, effect_depth_arrayed(state), FALSE, TRUE),
		STORAGE_UNIFORM_CONSTANT, "effect_depth");
	decorate1(m, state->effect_depth, DECORATION_DESCRIPTOR_SET, XGPU_SPIRV_SET_TEXTURES);
	decorate1(m, state->effect_depth, DECORATION_BINDING, XGPU_SPIRV_EFFECT_DEPTH_BINDING);
	state->effect_thickness = variable(m, type_sampled_image(m, DIM_2D, FALSE, FALSE, TRUE), STORAGE_UNIFORM_CONSTANT,
		"effect_thickness");
	decorate1(m, state->effect_thickness, DECORATION_DESCRIPTOR_SET, XGPU_SPIRV_SET_TEXTURES);
	decorate1(m, state->effect_thickness, DECORATION_BINDING, XGPU_SPIRV_EFFECT_THICKNESS_BINDING);
}

/* a depth the target holds (gl_FragCoord's z, or the depth buffer's) as
the game has it: the effects' unprojections are the game's (on reversed
depth, 1 - it) */
static unsigned int effect_game_depth(struct module *m, const struct xgpu_spirv_options *options, unsigned int depth)
{
	return options->reversed_depth ? fsub(m, 1, constant_float(m, 1.0f), depth) : depth;
}

/* the world point (a vec3) of a pixel's homogeneous unprojection */
static unsigned int effect_point(struct module *m, unsigned int homogeneous)
{
	return fdiv(m, 3, xyz(m, homogeneous), splat(m, 3, extract(m, homogeneous, 3)));
}

/* What an effect works out before its textures are sampled, in uniform
control flow (its derivatives): the world point the pixel draws, the
scene's behind it and the eye's distance to each; the size of the
sprite's texels in the world there (the world's derivatives over the
texels'), so its radius and thickness in world units; with its thickness,
texture 0's parallax (state->t0_offset). Returns the fade (1 drawn, 0
gone). As GLSL:

vec4 base = unproject[view][0] * gl_FragCoord.x + unproject[view][1] * gl_FragCoord.y + unproject[view][3];
vec3 p = point(base + unproject[view][2] * gl_FragCoord.z);
float depth = texelFetch(effect_depth, ivec3(gl_FragCoord.xy, view), 0).r;
(a flat screen's: depth = eye[1].z > 0.5 ? depth * eye[1].x + eye[1].y : depth)
vec4 behind = base + unproject[view][2] * depth;
float to_scene = depth >= 1.0 || behind.w <= 0.0 ? 1.0e9 : length(point(behind) - eye[view]);
float to_pixel = length(p - eye[view]); vec3 ray = (p - eye[view]) / to_pixel;
vec2 uv = xT0.xy / xT0.w, scaled = uv * texture_scale[0].xy;
vec4 thick = texture(effect_thickness, scaled);
vec2 texels = vec2(textureSize(tex0, 0));
float texel = sqrt((dot(dFdx(p), dFdx(p)) + dot(dFdy(p), dFdy(p))) /
	max(dot(dFdx(scaled) * texels, dFdx(scaled) * texels) + dot(dFdy(scaled) * texels, dFdy(scaled) * texels), 1e-20));
float radius = thick.g * UNIT * texel, front = to_pixel;
float span = max(max(SOFTNESS * radius, SOFTNESS_LEAST), SOFTNESS_DISTANCE * to_pixel);
thick:
	float height = thick.r * UNIT * THICKNESS * texel;
	vec3 n = normalize(cross(dFdx(p), dFdy(p))); n = dot(n, ray) > 0.0 ? -n : n;
	float along = height / max(-dot(ray, n), 0.25);   (along the ray, from the bulge's front to the sprite)
	vec3 delta = -ray * along - n * height;             (in the sprite's plane: where the front's texel is)
	(delta = a dFdx(p) + b dFdy(p), by the 2x2 of their dot products)
	t0_offset = a dFdx(uv) + b dFdy(uv), no longer than PARALLAX_MOST of the radius in texels;
	front = to_pixel - along; span = max(2.0 * along, span);
return clamp((to_scene - front) / span, 0.0, 1.0); */
static unsigned int effect_prelude(struct module *m, struct pixel_state *state)
{
	const struct xgpu_spirv_options *options = state->options;
	/* (a z-sprite's front is its own depth: effect_zsprite_fade) */
	BOOL thick = options->effect == XGPU_SPIRV_EFFECT_THICK && !xgpu_pixel_program_depth_stage(state->program);
	/* (a flat screen's: the first view, 0) */
	BOOL arrayed = effect_depth_arrayed(state);
	unsigned int view = arrayed ? view_number(m, XGPU_MULTIVIEW_EYES, options->foveated) : constant_int(m, 0);
	unsigned int frag = load(m, m->t_v4, state->frag_coord), columns[4], first_column, base, p, depth, behind;
	unsigned int eye, to_scene, far, to_pixel, offset_to_pixel, ray, xt, w, uv, scaled, thick_texel, texels;
	unsigned int dpx, dpy, dux, duy, texel, radius, front, span, operands[4];
	int column;

	first_column = op2(m, OP_I_MUL, m->t_int, view, constant_int(m, 4));
	for (column = 0; column < 4; column++)
		columns[column] = uniform_index(m, U_EFFECT_UNPROJECT, column ? op2(m, OP_I_ADD, m->t_int, first_column,
			constant_int(m, column)) : first_column);
	base = fadd(m, 4, fadd(m, 4, fscale(m, 4, columns[0], extract(m, frag, 0)), fscale(m, 4, columns[1],
		extract(m, frag, 1))), columns[3]);
	p = effect_point(m, fadd(m, 4, base, fscale(m, 4, columns[2], effect_game_depth(m, options, extract(m, frag, 2)))));
	/* the scene's depth behind the pixel, in the view's layer */
	{
		unsigned int sampled = load(m, type_sampled_image(m, DIM_2D, arrayed, FALSE, TRUE), state->effect_depth);
		unsigned int parts[2];

		parts[0] = op1(m, OP_CONVERT_F_TO_S, m->t_iv2, xy(m, frag));
		parts[1] = view;
		operands[0] = op(m, OP_IMAGE, type_sampled_image(m, DIM_2D, arrayed, FALSE, FALSE), 1, &sampled);
		operands[1] = arrayed ? op(m, OP_COMPOSITE_CONSTRUCT, m->t_iv3, 2, parts) : parts[0];
		operands[2] = IMAGE_OPERANDS_LOD;
		operands[3] = constant_int(m, 0);
		depth = effect_game_depth(m, options, extract(m, op(m, OP_IMAGE_FETCH, m->t_v4, 4, operands), 0));
	}
	/* (a flat screen's draw over the first-person weapon's pixels: their
	depth of its own projection, as the world's would have it, the
	uniforms' effect_eye[1] its scale and offset where its z is 1, else
	unused: xgpu_stream.h's xgpu_depth_remap, xgpu_vulkan_stream.c's
	effect_first_person_uniforms) */
	if (!arrayed)
	{
		unsigned int remap = uniform_index(m, U_EFFECT_EYE, constant_int(m, 1));

		depth = op3(m, OP_SELECT, m->t_float, compare(m, OP_F_ORD_GREATER_THAN, extract(m, remap, 2),
			constant_float(m, 0.5f)), fadd(m, 1, fmul(m, 1, depth, extract(m, remap, 0)), extract(m, remap, 1)), depth);
	}
	behind = fadd(m, 4, base, fscale(m, 4, columns[2], depth));
	eye = xyz(m, uniform_index(m, U_EFFECT_EYE, view));
	far = op2(m, OP_LOGICAL_OR, m->t_bool, compare(m, OP_F_ORD_GREATER_THAN_EQUAL, depth, constant_float(m, 1.0f)),
		compare(m, OP_F_ORD_LESS_THAN_EQUAL, extract(m, behind, 3), constant_float(m, 0.0f)));
	to_scene = op3(m, OP_SELECT, m->t_float, far, constant_float(m, 1.0e9f),
		ext1(m, GLSL_LENGTH, m->t_float, fsub(m, 3, effect_point(m, behind), eye)));
	offset_to_pixel = fsub(m, 3, p, eye);
	to_pixel = ext1(m, GLSL_LENGTH, m->t_float, offset_to_pixel);
	ray = fdiv(m, 3, offset_to_pixel, splat(m, 3, fmax_scalar(m, 1, to_pixel, 1.0e-6f)));
	/* the sprite's texel: its coordinates as texture 0 samples them */
	xt = pixel_input(m, state, state->in_t[0], m->t_v4);
	w = extract(m, xt, 3);
	w = op3(m, OP_SELECT, m->t_float, compare(m, OP_F_UNORD_NOT_EQUAL, w, constant_float(m, 0.0f)), w,
		constant_float(m, 1.0f));
	uv = fdiv(m, 2, xy(m, xt), splat(m, 2, w));
	scaled = fmul(m, 2, uv, xy(m, uniform(m, U_TEXTURE_SCALE, 0)));
	thick_texel = op2(m, OP_IMAGE_SAMPLE_IMPLICIT_LOD, m->t_v4,
		load(m, type_sampled_image(m, DIM_2D, FALSE, FALSE, TRUE), state->effect_thickness), scaled);
	{
		unsigned int image = texture_load(m, state, 0);
		unsigned int raw = op(m, OP_IMAGE, type_sampled_image(m, DIM_2D, FALSE, FALSE, FALSE), 1, &image);

		texels = op1(m, OP_CONVERT_S_TO_F, m->t_v2, image_size(m, raw, m->t_iv2));
	}
	/* the world's size of a texel there */
	dpx = op1(m, OP_DPDX, m->t_v3, p);
	dpy = op1(m, OP_DPDY, m->t_v3, p);
	dux = fmul(m, 2, op1(m, OP_DPDX, m->t_v2, scaled), texels);
	duy = fmul(m, 2, op1(m, OP_DPDY, m->t_v2, scaled), texels);
	texel = ext1(m, GLSL_SQRT, m->t_float, fdiv(m, 1, fadd(m, 1, fdot(m, dpx, dpx), fdot(m, dpy, dpy)),
		fmax_scalar(m, 1, fadd(m, 1, fdot(m, dux, dux), fdot(m, duy, duy)), 1.0e-20f)));
	radius = fmul(m, 1, fmul(m, 1, extract(m, thick_texel, 1), constant_float(m, THICKNESS_UNIT)), texel);
	span = ext2(m, GLSL_F_MAX, m->t_float, fmax_scalar(m, 1, fmul(m, 1, radius,
		constant_float(m, XGPU_SPIRV_EFFECT_SOFTNESS)), XGPU_SPIRV_EFFECT_SOFTNESS_LEAST),
		fmul(m, 1, to_pixel, constant_float(m, XGPU_SPIRV_EFFECT_SOFTNESS_DISTANCE)));
	front = to_pixel;
	if (thick)
	{
		unsigned int height, n, cosine, along, delta, g11, g12, g22, determinant, b1, b2, a, b, valid, offset;
		unsigned int offset_texels, length, most, shrink;

		height = fmul(m, 1, fmul(m, 1, extract(m, thick_texel, 0),
			constant_float(m, THICKNESS_UNIT * XGPU_SPIRV_EFFECT_THICKNESS)), texel);
		n = ext1(m, GLSL_NORMALIZE, m->t_v3, ext2(m, GLSL_CROSS, m->t_v3, dpx, dpy));
		n = select_n(m, 3, compare(m, OP_F_ORD_GREATER_THAN, fdot(m, n, ray), constant_float(m, 0.0f)), fneg(m, 3, n), n);
		cosine = fmax_scalar(m, 1, fneg(m, 1, fdot(m, ray, n)), 0.25f);
		along = fdiv(m, 1, height, cosine);
		delta = fsub(m, 3, fneg(m, 3, fscale(m, 3, ray, along)), fscale(m, 3, n, height));
		g11 = fdot(m, dpx, dpx);
		g12 = fdot(m, dpx, dpy);
		g22 = fdot(m, dpy, dpy);
		determinant = fsub(m, 1, fmul(m, 1, g11, g22), fmul(m, 1, g12, g12));
		valid = compare(m, OP_F_ORD_GREATER_THAN, determinant, fmul(m, 1, fmul(m, 1, g11, g22),
			constant_float(m, 1.0e-4f)));
		determinant = op3(m, OP_SELECT, m->t_float, valid, determinant, constant_float(m, 1.0f));
		b1 = fdot(m, delta, dpx);
		b2 = fdot(m, delta, dpy);
		a = fdiv(m, 1, fsub(m, 1, fmul(m, 1, g22, b1), fmul(m, 1, g12, b2)), determinant);
		b = fdiv(m, 1, fsub(m, 1, fmul(m, 1, g11, b2), fmul(m, 1, g12, b1)), determinant);
		offset = fadd(m, 2, fscale(m, 2, op1(m, OP_DPDX, m->t_v2, uv), a), fscale(m, 2, op1(m, OP_DPDY, m->t_v2, uv), b));
		/* (no farther than PARALLAX_MOST of the sprite's radius: a sprite
		seen edge-on, or whose texels' derivatives fail) */
		offset_texels = fmul(m, 2, fmul(m, 2, offset, xy(m, uniform(m, U_TEXTURE_SCALE, 0))), texels);
		length = ext1(m, GLSL_LENGTH, m->t_float, offset_texels);
		most = fmul(m, 1, extract(m, thick_texel, 1), constant_float(m, THICKNESS_UNIT * XGPU_SPIRV_EFFECT_PARALLAX_MOST));
		shrink = ext2(m, GLSL_F_MIN, m->t_float, constant_float(m, 1.0f),
			fdiv(m, 1, most, fmax_scalar(m, 1, length, 1.0e-6f)));
		state->t0_offset = select_n(m, 2, valid, fscale(m, 2, offset, shrink), constant_splat(m, 2, 0.0f));
		front = fsub(m, 1, to_pixel, along);
		span = ext2(m, GLSL_F_MAX, m->t_float, fmul(m, 1, along, constant_float(m, 2.0f)), span);
	}
	state->effect_base = base;
	state->effect_column2 = columns[2];
	state->effect_eye = eye;
	state->effect_to_scene = to_scene;
	state->effect_span = span;
	return saturate(m, 1, fdiv(m, 1, fsub(m, 1, to_scene, front), span));
}

/* a z-sprite's fade as an effect, after its stages: measured from the puff's
front, the depth its DOT_ZW stage computes (no farther than its card), not
from the card; its distance along the eye's ray from the prelude's
unprojection. Its pipeline tests no depth (xgpu_vulkan_stream.c), as the
puff is nearer than the card the scene's depth would cut: the fade is its
test, against the scene's depth sampled, so where the front is behind the
scene it is discarded (effect_zsprite_hidden), whatever the blend's fade.
As GLSL:

float front_depth = min(replaced_depth, gl_FragCoord.z);
float front = length(point(base + unproject[view][2] * front_depth) - eye[view]);
fade = clamp((to_scene - front) / span, 0.0, 1.0);    hidden = front >= to_scene */
static unsigned int effect_zsprite_fade(struct module *m, struct pixel_state *state, unsigned int *hidden)
{
	unsigned int frag = load(m, m->t_v4, state->frag_coord);
	unsigned int front_depth = ext2(m, GLSL_F_MIN, m->t_float, state->replaced_depth,
		effect_game_depth(m, state->options, extract(m, frag, 2)));
	unsigned int front = ext1(m, GLSL_LENGTH, m->t_float, fsub(m, 3, effect_point(m, fadd(m, 4, state->effect_base,
		fscale(m, 4, state->effect_column2, front_depth))), state->effect_eye));

	*hidden = compare(m, OP_F_ORD_GREATER_THAN_EQUAL, front, state->effect_to_scene);
	return saturate(m, 1, fdiv(m, 1, fsub(m, 1, state->effect_to_scene, front), state->effect_span));
}

/* the effect's fade drawn as its blend's nothing (XGPU_SPIRV_FADE_*) */
static unsigned int effect_fade(struct module *m, int fade_mode, unsigned int result, unsigned int fade)
{
	static const unsigned int rgb_a[4] = { 4, 5, 6, 3 };
	unsigned int rgb = xyz(m, result);

	switch (fade_mode)
	{
	case XGPU_SPIRV_FADE_ALPHA:
		return op3(m, OP_COMPOSITE_INSERT, m->t_v4, fmul(m, 1, extract(m, result, 3), fade), result, 3);
	case XGPU_SPIRV_FADE_COLOR:
		return shuffle(m, 4, result, fscale(m, 3, rgb, fade), rgb_a);
	case XGPU_SPIRV_FADE_COLOR_ALPHA:
		return fscale(m, 4, result, fade);
	case XGPU_SPIRV_FADE_TO_WHITE:
	case XGPU_SPIRV_FADE_TO_GREY:
	{
		unsigned int neutral = constant_splat(m, 3, fade_mode == XGPU_SPIRV_FADE_TO_WHITE ? 1.0f : 0.5f);

		return shuffle(m, 4, result, ext3(m, GLSL_F_MIX, m->t_v3, neutral, rgb, splat(m, 3, fade)), rgb_a);
	}
	default:
		return result;
	}
}

BOOL xgpu_spirv_pixel_shader(const struct xgpu_pixel_program *program, const struct xgpu_spirv_options *options,
	struct xgpu_spirv_module *module)
{
	static const char *const coordinate_names[] = { "xT0", "xT1", "xT2", "xT3" };
	static const char *const texture_names[] = { "tex0", "tex1", "tex2", "tex3" };
	struct module m;
	struct pixel_state state;
	BOOL environment = program->dynamic_lights == XGPU_DYNAMIC_LIGHTS_ENVIRONMENT;
	/* (display.ambient_occlusion "world": the light the occlusion found in
	the world multiplies, at the pixel's position: xgpu.h; none without
	XGPU_AMBIENT_OCCLUSION_WORLD) */
	int occlusion_light = (program->ambient_occlusion & XGPU_AMBIENT_OCCLUSION_WORLD) ?
		XGPU_AMBIENT_OCCLUSION_LIGHT_OF(program->ambient_occlusion) : XGPU_AMBIENT_OCCLUSION_LIGHT_NONE;
	BOOL world = occlusion_light != XGPU_AMBIENT_OCCLUSION_LIGHT_NONE;
	/* (found once a pixel: read from its texture, or the pre-pass's inputs
	written in the colour's place: xgpu.h) */
	BOOL world_inputs = world && (program->ambient_occlusion & XGPU_AMBIENT_OCCLUSION_WORLD_INPUTS);
	BOOL world_sampled = world && !world_inputs && (program->ambient_occlusion & XGPU_AMBIENT_OCCLUSION_WORLD_SAMPLED);
	BOOL world_inline = world && !world_inputs && !world_sampled;
	/* (an occluder's twin in the pre-pass: no inputs, where its alpha test
	keeps it) */
	BOOL occluder = !world && (program->ambient_occlusion & XGPU_AMBIENT_OCCLUSION_WORLD_INPUTS);
	unsigned int world_ao_words = 0;
	/* (HALO_LT_E1: the twin's guides' output, and the shaded normal the dynamic lights use: 0 none) */
	unsigned int lt_guides_out = 0, lt_shaded_normal = 0;
	/* (its motion's positions, this frame's and the frame before's: 0 none) */
	unsigned int lt_motion_current = 0, lt_motion_previous = 0;
	BOOL world_position = (program->per_pixel_lighting && ((program->per_pixel_lighting & 3) == 2 ||
		program->dynamic_lights == XGPU_DYNAMIC_LIGHTS_MODEL)) || environment || world || program->rt_reflection;
	BOOL count_samples = program->count_samples && options->count_samples;
	unsigned int comparison = program->alpha_test ? comparison_opcode(program->alpha_test) : 0;
	BOOL sample_mask = comparison && program->alpha_test_samples && program->alpha_test != D3DCMP_EQUAL &&
		program->alpha_test != D3DCMP_NOTEQUAL;
	unsigned int main_id, color_out, sample_mask_out = 0, result, dynamic_light_bump = 0, shine_variable = 0, highlight_scale = 0, effect_fade_value = 0;
	BOOL effect = options->effect != XGPU_SPIRV_EFFECT_NONE && xgpu_spirv_effect_possible(program);
	/* a z-sprite (xgpu_shader.h): its depth written, conservatively (only
	ever nearer than the card), or as a VR effect its fade measured from it */
	BOOL replaces_depth = xgpu_pixel_program_depth_stage(program) != 0;
	unsigned int frag_depth_out = 0, zsprite_hidden = 0;
	BOOL interlocked = FALSE;
	struct hdr_store target;
	unsigned int drawn_over = 0;
	/* (display.hdr: a self-illuminated surface's pass; its pixel without its
	self-illumination, and the light that adds past white) */
	BOOL self_illuminated = XGPU_HDR_IS_SELF_ILLUMINATED(program->hdr) && !program->source_blend;
	unsigned int self_illumination_base = 0, self_illumination = 0;
	unsigned int in_motion_current = 0, in_motion_previous = 0, motion_out = 0;
	BOOL lt_forward;
	int stage;

	lt_shader_on = options->light_transport != 0;
	/* (the one light model's forward receiver: lit by the dynamic lights,
	with no pass light of its own, nor a twin or an effect: xgpu_spirv.h's
	XGPU_SPIRV_BINDING_LT_TABLE) */
	lt_forward = lt_shader_on && program->dynamic_lights && !world_sampled && !world_inputs && !occluder && !effect;

	memset(&state, 0, sizeof(state));
	state.program = program;
	state.options = options;
	module_begin(&m, EXECUTION_MODEL_FRAGMENT);
	if (world || environment || lt_forward)
		declare_uniforms_world(&m, environment);
	else
		declare_uniforms(&m);
	declare_views(&m, program->multiview, options->foveated, TRUE);
	if (program->dynamic_lights)
		declare_dynamic_lights(&m);
	for (stage = 0; stage < 4; stage++)
	{
		unsigned char sampler = program->stages[stage].sampler;

		if (!stage_samples(program->stages[stage].mode))
			continue;
		state.textures[stage] = variable(&m, type_sampled_image(&m, sampler == _xgpu_sampler_3d ? DIM_3D :
			sampler == _xgpu_sampler_cube ? DIM_CUBE : DIM_2D, sampler == _xgpu_sampler_2d_layered, FALSE, TRUE),
			STORAGE_UNIFORM_CONSTANT, texture_names[stage]);
		decorate1(&m, state.textures[stage], DECORATION_DESCRIPTOR_SET, XGPU_SPIRV_SET_TEXTURES);
		decorate1(&m, state.textures[stage], DECORATION_BINDING, stage);
	}
	/* (a model's material maps: material_read) */
	if (program->material_maps & XGPU_TEXTURE_MODES_NORMAL_MAP)
	{
		state.normal_map = variable(&m, type_sampled_image(&m, DIM_2D, FALSE, FALSE, TRUE), STORAGE_UNIFORM_CONSTANT,
			"normal_map");
		decorate1(&m, state.normal_map, DECORATION_DESCRIPTOR_SET, XGPU_SPIRV_SET_TEXTURES);
		decorate1(&m, state.normal_map, DECORATION_BINDING, XGPU_SPIRV_BINDING_NORMAL_MAP);
	}
	if (program->material_maps & XGPU_TEXTURE_MODES_ORM_MAP)
	{
		state.orm_map = variable(&m, type_sampled_image(&m, DIM_2D, FALSE, FALSE, TRUE), STORAGE_UNIFORM_CONSTANT,
			"orm_map");
		decorate1(&m, state.orm_map, DECORATION_DESCRIPTOR_SET, XGPU_SPIRV_SET_TEXTURES);
		decorate1(&m, state.orm_map, DECORATION_BINDING, XGPU_SPIRV_BINDING_ORM_MAP);
	}
	if (effect)
		effect_declare(&m, &state);
	else if (replaces_depth)
	{
		state.frag_coord = variable(&m, m.t_v4, STORAGE_INPUT, "gl_FragCoord");
		decorate1(&m, state.frag_coord, DECORATION_BUILT_IN, BUILT_IN_FRAG_COORD);
		frag_depth_out = variable(&m, m.t_float, STORAGE_OUTPUT, "gl_FragDepth");
		decorate1(&m, frag_depth_out, DECORATION_BUILT_IN, BUILT_IN_FRAG_DEPTH);
	}
	if (world_inline)
		world_ao_declare(&m);
	else if (world_inputs)
		world_ao_declare(&m);
	else if (world_sampled)
	{
		world_ao_declare(&m);
		world_ao_declare_pixels(&m);
		if (!state.frag_coord)
		{
			state.frag_coord = variable(&m, m.t_v4, STORAGE_INPUT, "gl_FragCoord");
			decorate1(&m, state.frag_coord, DECORATION_BUILT_IN, BUILT_IN_FRAG_COORD);
		}
		if (program->dynamic_lights)
		{
			light_shadows_declare(&m, &state);
			rt_bounce_declare(&m, &state); /* display.rt_bounce */
		}
	}
	if (lt_forward)
	{
		state.lt_forward = TRUE;
		lt_table_declare(&m, &state);
	}
	/* (the one light model's shine on a model with a reflection, lt_ggx: the
	pass's light table read by the pre-pass's models too) */
	else if (lt_shader_on && program->dynamic_lights == XGPU_DYNAMIC_LIGHTS_MODEL && program->per_pixel_lighting &&
		program->stages[3].mode == XGPU_TEXTURE_CUBEMAP && !world_inputs && !occluder && !effect)
		lt_table_declare(&m, &state);
	if (count_samples)
	{
		unsigned int array = type1(&m, OP_TYPE_RUNTIME_ARRAY, m.t_uint), block;

		decorate1(&m, array, DECORATION_ARRAY_STRIDE, 4);
		block = type_struct(&m, 1, &array);

		decorate(&m, block, DECORATION_BLOCK, 0, NULL);
		member_decorate(&m, block, 0, DECORATION_OFFSET, 0);
		name(&m, block, "visible_samples_block");
		m.samples = variable(&m, block, STORAGE_STORAGE_BUFFER, "visible_samples");
		decorate1(&m, m.samples, DECORATION_DESCRIPTOR_SET, XGPU_SPIRV_SET_DRAW);
		decorate1(&m, m.samples, DECORATION_BINDING, XGPU_SPIRV_BINDING_SAMPLES);
	}

	/* the inputs */
	state.in_d0 = interface_variable(&m, m.t_v4, STORAGE_INPUT, XGPU_SPIRV_LOCATION_D0, "xD0");
	state.in_d1 = interface_variable(&m, m.t_v4, STORAGE_INPUT, XGPU_SPIRV_LOCATION_D1, "xD1");
	for (stage = 0; stage < 4; stage++)
		state.in_t[stage] = interface_variable(&m, m.t_v4, STORAGE_INPUT, XGPU_SPIRV_LOCATION_T0 + stage, coordinate_names[stage]);
	state.in_fog = interface_variable(&m, m.t_float, STORAGE_INPUT, XGPU_SPIRV_LOCATION_FOG, "xFog");
	if (occlusion_light == XGPU_AMBIENT_OCCLUSION_LIGHT_VERTEX ||
		(program->per_pixel_lighting & XGPU_PER_PIXEL_VERTEX_AMBIENT))
	{
		state.in_light_ambient = interface_variable(&m, m.t_v3, STORAGE_INPUT, XGPU_SPIRV_LOCATION_LIGHT_AMBIENT,
			"xLightAmbient");
		state.in_light_point = interface_variable(&m, m.t_v3, STORAGE_INPUT, XGPU_SPIRV_LOCATION_LIGHT_POINT,
			"xLightPoint");
	}
	if (program->per_pixel_lighting || environment || world)
		state.in_world_normal = interface_variable(&m, m.t_v4, STORAGE_INPUT, XGPU_SPIRV_LOCATION_WORLD_NORMAL, "xWorldNormal");
	if (world_position)
		state.in_world_position = interface_variable(&m, m.t_v3, STORAGE_INPUT, XGPU_SPIRV_LOCATION_WORLD_POSITION, "xWorldPosition");
	/* (display.rt_reflections: not in the pre-pass's twins, which write no
	colour, nor a VR effect's) */
	state.rt_reflection = program->rt_reflection && state.in_world_position && !world_inputs && !occluder && !effect;
	if (environment)
	{
		state.in_world_binormal = interface_variable(&m, m.t_v3, STORAGE_INPUT, XGPU_SPIRV_LOCATION_WORLD_BINORMAL, "xWorldBinormal");
		state.in_world_tangent = interface_variable(&m, m.t_v3, STORAGE_INPUT, XGPU_SPIRV_LOCATION_WORLD_TANGENT, "xWorldTangent");
	}
	color_out = interface_variable(&m, world_inputs || occluder ? m.t_uv4 : m.t_v4, STORAGE_OUTPUT, 0, "fragment_color");
	if (world_inputs && lt_e1_on())
		lt_guides_out = interface_variable(&m, m.t_uv4, STORAGE_OUTPUT, XGPU_SPIRV_GUIDES_LOCATION, "lt_guides");
	/* (and the twin's motion, where its vertex shader hands it on: xgpu_spirv.h's C1) */
	if (lt_guides_out && options->motion && !program->multiview)
	{
		lt_motion_current = interface_variable(&m, m.t_v3, STORAGE_INPUT, XGPU_SPIRV_LOCATION_MOTION_CURRENT,
			"xMotionCurrent");
		lt_motion_previous = interface_variable(&m, m.t_v3, STORAGE_INPUT, XGPU_SPIRV_LOCATION_MOTION_PREVIOUS,
			"xMotionPrevious");
	}
	/* (display.temporal_aa: the motion, the second attachment's: the
	previous position less the current, as a fraction of the view, x right
	and y down, and the pixel's depth, which the resolve finds it still is) */
	if (options->motion && !program->multiview && !world_inputs && !occluder)
	{
		in_motion_current = interface_variable(&m, m.t_v3, STORAGE_INPUT, XGPU_SPIRV_LOCATION_MOTION_CURRENT,
			"xMotionCurrent");
		in_motion_previous = interface_variable(&m, m.t_v3, STORAGE_INPUT, XGPU_SPIRV_LOCATION_MOTION_PREVIOUS,
			"xMotionPrevious");
		motion_out = interface_variable(&m, m.t_v4, STORAGE_OUTPUT, 1, "fragment_motion");
		if (!state.frag_coord)
		{
			state.frag_coord = variable(&m, m.t_v4, STORAGE_INPUT, "gl_FragCoord");
			decorate1(&m, state.frag_coord, DECORATION_BUILT_IN, BUILT_IN_FRAG_COORD);
		}
	}
	if (sample_mask)
	{
		unsigned int operands[3], array = id_new(&m);

		operands[0] = array;
		operands[1] = m.t_int;
		operands[2] = constant_uint(&m, 1);
		instruction(&m, SECTION_GLOBALS, OP_TYPE_ARRAY, 3, operands);
		sample_mask_out = variable(&m, array, STORAGE_OUTPUT, "gl_SampleMask");
		decorate1(&m, sample_mask_out, DECORATION_BUILT_IN, BUILT_IN_SAMPLE_MASK);
	}

	main_id = function_begin(&m);
	state.zero4 = constant_splat(&m, 4, 0.0f);
	state.v0 = pixel_input(&m, &state, state.in_d0, m.t_v4);
	state.v1 = pixel_input(&m, &state, state.in_d1, m.t_v4);
	for (stage = 0; stage < 4; stage++)
	{
		state.t[stage] = state.zero4;
		state.dots[stage] = constant_float(&m, 0.0f);
	}
	/* display.ambient_occlusion "world": the occlusion at the pixel, which
	multiplies the ambient light alone (xgpu.h) */
	if (world_inline)
		state.occlusion = world_ao_read(&m, &state, occlusion_light);
	else if (world_sampled)
		state.occlusion = world_ao_upsample(&m, &state);
	else if (world_inputs)
	{
		state.occlusion = constant_float(&m, 1.0f);
		world_ao_words = world_ao_inputs(&m, &state, occlusion_light);
	}
	if (program->per_pixel_lighting && program->dynamic_lights == XGPU_DYNAMIC_LIGHTS_MODEL)
	{
		unsigned int world_normal = pixel_input(&m, &state, state.in_world_normal, m.t_v4), normal = xyz(&m, world_normal);
		unsigned int n = fscale(&m, 3, normal, ext1(&m, GLSL_INVERSE_SQRT, m.t_float,
			fmax_scalar(&m, 1, fdot(&m, normal, normal), (float)1.0e-24)));
		unsigned int p = pixel_input(&m, &state, state.in_world_position, m.t_v3);
		unsigned int lit, occlusion = occlusion_light == XGPU_AMBIENT_OCCLUSION_LIGHT_MODEL ? state.occlusion : 0;
		unsigned int reflection = 0, exponent = 0, sum, shaded = n, added, linear_variable;

		lt_shaded_normal = n;
		static const unsigned int rgb[4] = { 4, 5, 6, 3 };

		/* (a model's material maps: its normal, and its occlusion with the
		ambient occlusion's) */
		if (program->material_maps && state.in_t[0])
		{
			material_read(&m, &state, p, n);
			if (state.shaded_normal)
				shaded = state.shaded_normal;
			lt_shaded_normal = shaded;
			if (state.material_occlusion)
				occlusion = occlusion ? fmul(&m, 1, occlusion, state.material_occlusion) : state.material_occlusion;
		}
		/* (its ambient and distant lights the vertex's, as the game's program
		gives them, by the occlusion: xgpu.h's XGPU_PER_PIXEL_VERTEX_AMBIENT;
		else for each pixel) */
		if (program->per_pixel_lighting & XGPU_PER_PIXEL_VERTEX_AMBIENT)
		{
			lit = pixel_input(&m, &state, state.in_light_ambient, m.t_v3);
			if (occlusion)
				lit = fscale(&m, 3, lit, occlusion);
			lit = saturate(&m, 3, lit);
		}
		else
			lit = model_lighting(&m, &state, (program->per_pixel_lighting & 3) == 2, TRUE, occlusion);

		if (program->stages[3].mode == XGPU_TEXTURE_CUBEMAP && state.in_t[3])
		{
			/* (its reflection's highlights of the lights: xgpu.h
			XGPU_DYNAMIC_LIGHTS_HIGHLIGHT) */
			unsigned int to_eye = fsub(&m, 3, xyz(&m, dynamic_light(&m, 0, constant_int(&m, 0))), p);
			unsigned int v = fscale(&m, 3, to_eye, ext1(&m, GLSL_INVERSE_SQRT, m.t_float,
				fmax_scalar(&m, 1, fdot(&m, to_eye, to_eye), (float)1.0e-12)));
			unsigned int dx, dy, widened;
			const float lobe = 2.0f / (XGPU_DYNAMIC_LIGHTS_HIGHLIGHT_EXPONENT + 2.0f);

			/* reflection = 2.0 * dot(n, v) * n - v (the normal map's n, where it has one) */
			reflection = fsub(&m, 3, fscale(&m, 3, shaded, fmul(&m, 1, fdot(&m, shaded, v), constant_float(&m, 2.0f))), v);
			/* widened = lobe + min(dot(dFdx(r), dFdx(r)) + dot(dFdy(r), dFdy(r)), WIDEST);
			exponent = 2.0 / widened - 2.0; the shine by lobe / widened */
			dx = op1(&m, OP_DPDX, m.t_v3, reflection);
			dy = op1(&m, OP_DPDY, m.t_v3, reflection);
			widened = fadd(&m, 1, constant_float(&m, lobe), ext2(&m, GLSL_F_MIN, m.t_float,
				fadd(&m, 1, fdot(&m, dx, dx), fdot(&m, dy, dy)), constant_float(&m, XGPU_DYNAMIC_LIGHTS_HIGHLIGHT_WIDEST)));
			/* (... and by the material's roughness: widened += roughness^2 * ROUGH) */
			if (state.roughness)
				widened = fadd(&m, 1, widened, fmul(&m, 1, fmul(&m, 1, state.roughness, state.roughness),
					constant_float(&m, MATERIAL_HIGHLIGHT_ROUGH)));
			exponent = fsub(&m, 1, fdiv(&m, 1, constant_float(&m, 2.0f), widened), constant_float(&m, 2.0f));
			highlight_scale = fdiv(&m, 1, constant_float(&m, lobe * XGPU_DYNAMIC_LIGHTS_HIGHLIGHT), widened);
			shine_variable = local(&m, &state.locals, m.t_v3);
			store(&m, shine_variable, constant_splat(&m, 3, 0.0f));
		}
		/* (the classic light model without the ray-traced lighting:
		display.ray_traced_lighting, lt_light_model) */
		if (!lt_light_model())
		{
			sum = dynamic_light_sum_classic(&m, &state, p, shaded, n, FALSE, reflection, exponent, shine_variable);
			added = dynamic_light_add_classic(&m, &state, lit, sum, FALSE);
		}
		else
		{
			/* (the shine, lt_ggx: a model with a reflection, by its roughness
			map's roughness, else LT_MODEL_ROUGHNESS) */
			if (state.lt_table && shine_variable)
			{
				unsigned int to_eye = fsub(&m, 3, xyz(&m, dynamic_light(&m, 0, constant_int(&m, 0))), p);
				unsigned int roughness = state.roughness ? fmax_scalar(&m, 1, state.roughness, LT_MODEL_ROUGHEST_SMOOTH) :
					constant_float(&m, LT_MODEL_ROUGHNESS);

				state.lt_spec = local(&m, &state.locals, m.t_v3);
				store(&m, state.lt_spec, constant_splat(&m, 3, 0.0f));
				state.lt_spec_v = fscale(&m, 3, to_eye, ext1(&m, GLSL_INVERSE_SQRT, m.t_float,
					fmax_scalar(&m, 1, fdot(&m, to_eye, to_eye), (float)1.0e-12)));
				state.lt_spec_alpha = fmul(&m, 1, roughness, roughness);
				/* (a model the pre-pass drew: the pass lit its diffuse light, so
				only the loop's shine is kept here) */
				if (!state.lt_forward)
					(void)lt_forward_light(&m, &state, p, n, shaded, FALSE);
			}
			linear_variable = local(&m, &state.locals, m.t_v3);
			/* (linear_variable: the linear light added past the game's lights'
			gamma sum, dynamic_light_add's linear; another linear light, a
			bounce's, adds into it before the add) */
			sum = dynamic_light_sum(&m, &state, p, shaded, n, FALSE, reflection, exponent, shine_variable,
				linear_variable);
			/* display.rt_bounce: the bounced light, linear, into the linear sum
			(HALO_LT_E1: the traced flashlight there, added after, linearly) */
			if (!lt_e1_on())
				store(&m, linear_variable, rt_bounce_add(&m, &state, load(&m, m.t_v3, linear_variable)));
			added = dynamic_light_add(&m, &state, lit, sum, load(&m, m.t_v3, linear_variable), FALSE);
			added = lt_e1_add(&m, &state, added, 0, p, shaded);
			/* (a forward receiver: the pass's light found here) */
			added = lt_forward_add(&m, &state, added, 0, p, n, shaded);
		}
		/* (a metal's diffuse light darkened: material_read; by half with the
		one light model too: the first-person gun's reflection is still its
		cube map, not the scene's light, and a black metal read as unlit) */
		if (state.metalness)
			added = fscale(&m, 3, added, fsub(&m, 1, constant_float(&m, 1.0f), fmul(&m, 1, state.metalness,
				constant_float(&m, MATERIAL_METAL_DIFFUSE))));
		/* (a glowing part, the MCC import's emissive (the ORM map's alpha,
		mod_materials.c): with the one light model, as much light again as
		the base map's colour at full light, LT_EMISSIVE_LIGHT, added after
		the metal's darkening (a glow is not a reflection)) */
		if (state.material_emissive)
			added = ext2(&m, GLSL_POW, m.t_v3, fadd(&m, 3, ext2(&m, GLSL_POW, m.t_v3, fmax_scalar(&m, 3, added, 0.0f),
				constant_splat(&m, 3, 2.2f)), splat(&m, 3, fmul(&m, 1, state.material_emissive,
				constant_float(&m, LT_EMISSIVE_LIGHT)))), constant_splat(&m, 3, 1.0f / 2.2f));
		state.v0 = shuffle(&m, 4, state.v0, added, rgb);
	}
	else if (program->per_pixel_lighting)
	{
		static const unsigned int rgb[4] = { 4, 5, 6, 3 };

		state.v0 = shuffle(&m, 4, state.v0, model_lighting(&m, &state, (program->per_pixel_lighting & 3) == 2, FALSE,
			occlusion_light == XGPU_AMBIENT_OCCLUSION_LIGHT_MODEL ? state.occlusion : 0), rgb);
	}
	else if (occlusion_light == XGPU_AMBIENT_OCCLUSION_LIGHT_VERTEX)
	{
		/* (lit for each vertex: its ambient light by the occlusion, its
		point lights' added, clamped as its program clamps the sum) */
		static const unsigned int rgb[4] = { 4, 5, 6, 3 };
		unsigned int lit = saturate(&m, 3, fadd(&m, 3,
			fscale(&m, 3, pixel_input(&m, &state, state.in_light_ambient, m.t_v3), state.occlusion),
			pixel_input(&m, &state, state.in_light_point, m.t_v3)));

		state.v0 = shuffle(&m, 4, state.v0, lit, rgb);
	}

	if (effect)
		effect_fade_value = effect_prelude(&m, &state);
	for (stage = 0; stage < 4; stage++)
		texture_stage(&m, &state, stage);
	/* (a model's highlights of the lights, added to its reflection's texel:
	xgpu.h XGPU_DYNAMIC_LIGHTS_HIGHLIGHT) */
	if (shine_variable)
	{
		static const unsigned int rgb[4] = { 4, 5, 6, 3 };

		state.t[3] = shuffle(&m, 4, state.t[3], !lt_light_model() ? dynamic_light_add_classic(&m, &state,
			xyz(&m, state.t[3]), fscale(&m, 3, load(&m, m.t_v3, shine_variable), highlight_scale), FALSE) :
			dynamic_light_add(&m, &state, xyz(&m, state.t[3]), constant_splat(&m, 3, 0.0f),
			fscale(&m, 3, load(&m, m.t_v3, shine_variable), highlight_scale), FALSE), rgb);
	}
	/* (the one light model's shine, lt_ggx, added to the reflection's texel
	linearly, as lt_e1_add adds light) */
	if (state.lt_spec)
	{
		static const unsigned int rgb[4] = { 4, 5, 6, 3 };
		/* (HALO_LT_SHINE_VIEW=1, checking: the reflection's texel the shine
		alone, the cube map left out) */
		unsigned int under = lt_shine_view() ? constant_splat(&m, 3, 0.0f) : ext2(&m, GLSL_POW, m.t_v3,
			fmax_scalar(&m, 3, xyz(&m, state.t[3]), 0.0f), constant_splat(&m, 3, 2.2f));

		state.t[3] = shuffle(&m, 4, state.t[3], ext2(&m, GLSL_POW, m.t_v3, fadd(&m, 3, under,
			fmax_scalar(&m, 3, load(&m, m.t_v3, state.lt_spec), 0.0f)), constant_splat(&m, 3, 1.0f / 2.2f)), rgb);
	}
	/* (a model's material: its reflection by its occlusion, and a metal's
	tinted by its base map) */
	if ((state.material_occlusion || state.metalness) && program->stages[3].mode == XGPU_TEXTURE_CUBEMAP)
	{
		static const unsigned int rgb[4] = { 4, 5, 6, 3 };
		unsigned int reflected = xyz(&m, state.t[3]);

		if (state.metalness)
			reflected = ext3(&m, GLSL_F_MIX, m.t_v3, reflected, fmul(&m, 3, reflected, xyz(&m, state.t[0])),
				splat(&m, 3, state.metalness));
		if (state.material_occlusion)
			reflected = fscale(&m, 3, reflected, state.material_occlusion);
		state.t[3] = shuffle(&m, 4, state.t[3], reflected, rgb);
	}
	if (effect && replaces_depth && state.replaced_depth)
		effect_fade_value = effect_zsprite_fade(&m, &state, &zsprite_hidden);
	/* (the bump map's texel, which the combiners may overwrite: the enhanced
	dynamic lights read it after them) */
	if (environment)
		dynamic_light_bump = xyz(&m, state.t[0]);

	state.r0 = vec4_of(&m, constant_splat(&m, 3, 0.0f), extract(&m, state.t[0], 3));
	state.r1 = state.zero4;
	/* (display.hdr: a self-illuminated surface's pixel without its
	self-illumination, the registers as they were after) */
	if (self_illuminated)
	{
		unsigned int v0 = state.v0, v1 = state.v1, t[4], r0 = state.r0, r1 = state.r1;

		memcpy(t, state.t, sizeof(t));
		state.zeroed_stages = XGPU_HDR_SELF_ILLUMINATION_STAGES(program->hdr);
		self_illumination_base = saturate(&m, 3, xyz(&m, combiners(&m, &state)));
		state.zeroed_stages = 0;
		state.v0 = v0;
		state.v1 = v1;
		memcpy(state.t, t, sizeof(t));
		state.r0 = r0;
		state.r1 = r1;
	}
	result = combiners(&m, &state);
	/* (... and the light its self-illumination adds past white, toward its
	ceiling where it is bright, as a glow's: xgpu_hdr_self_illuminate) */
	if (self_illuminated)
	{
		unsigned int lit = saturate(&m, 3, xyz(&m, result));
		unsigned int added = fmax_scalar(&m, 3, fsub(&m, 3, lit, self_illumination_base), 0.0f);
		unsigned int added_most = brightest_of(&m, added), most = brightest_of(&m, lit), more;

		more = ext2(&m, GLSL_F_MIN, m.t_float, fdiv(&m, 1, fmul(&m, 1, fmul(&m, 1,
			constant_float(&m, XGPU_HDR_SELF_ILLUMINATION_VALUE - 1.0f), ext3(&m, GLSL_SMOOTH_STEP, m.t_float,
			constant_float(&m, XGPU_HDR_SELF_ILLUMINATION_KNEE), constant_float(&m, 1.0f), most)), most),
			fmax_scalar(&m, 1, added_most, 1.0e-6f)), constant_float(&m, XGPU_HDR_EXPAND_MOST));
		more = op3(&m, OP_SELECT, m.t_float, compare(&m, OP_F_ORD_GREATER_THAN, added_most, constant_float(&m, 0.0f)),
			more, constant_float(&m, 0.0f));
		self_illumination = fscale(&m, 3, added, more);
		/* (... gained by display.hdr_highlight_boost's level, toward the
		same ceiling: xgpu_hdr_self_illuminate_boost) */
		if (XGPU_HDR_BOOST_OF(program->hdr))
			self_illumination = fadd(&m, 3, self_illumination, boost_extra(&m, fadd(&m, 3, lit, self_illumination),
				fadd(&m, 3, added, self_illumination), XGPU_HDR_BOOST_OF(program->hdr),
				XGPU_HDR_SELF_ILLUMINATION_VALUE));
	}
	/* (display.ambient_occlusion: a lightmap pass's light, its indirect
	share by the occlusion, before the enhanced dynamic lights add theirs:
	linear, light - (1 - occlusion) * min(light, INDIRECT); xgpu.h's
	XGPU_AMBIENT_OCCLUSION_INDIRECT) */
	if (occlusion_light == XGPU_AMBIENT_OCCLUSION_LIGHT_LIGHTMAP && !lt_light_model())
	{
		static const unsigned int rgb[4] = { 4, 5, 6, 3 };

		/* (classic: the light by the occlusion, without the ray-traced
		lighting: lt_light_model) */
		result = shuffle(&m, 4, result, fscale(&m, 3, xyz(&m, result), state.occlusion), rgb);
	}
	else if (occlusion_light == XGPU_AMBIENT_OCCLUSION_LIGHT_LIGHTMAP)
	{
		static const unsigned int rgb[4] = { 4, 5, 6, 3 };
		unsigned int light = ext2(&m, GLSL_POW, m.t_v3, fmax_scalar(&m, 3, xyz(&m, result), 0.0f),
			constant_splat(&m, 3, 2.2f));
		/* (the level outdoors, the block's grid[0].w; 0 an interior's: all of it) */
		unsigned int level = extract(&m, world_ao_vector(&m, 3, constant_int(&m, 0)), 3);
		unsigned int indirect = op3(&m, OP_SELECT, m.t_v3, op2(&m, OP_F_ORD_GREATER_THAN, m.t_bv3, splat(&m, 3, level),
			constant_splat(&m, 3, 0.0f)), ext2(&m, GLSL_F_MIN, m.t_v3, light, splat(&m, 3, level)), light);

		/* (the occlusion as it multiplied the gamma light, occlusion^2.2 of
		the linear: an interior's light as it was) */
		light = fsub(&m, 3, light, fscale(&m, 3, indirect, fsub(&m, 1, constant_float(&m, 1.0f),
			ext2(&m, GLSL_POW, m.t_float, fmax_scalar(&m, 1, state.occlusion, 0.0f), constant_float(&m, 2.2f)))));
		result = shuffle(&m, 4, result, ext2(&m, GLSL_POW, m.t_v3, fmax_scalar(&m, 3, light, 0.0f),
			constant_splat(&m, 3, (float)(1.0 / 2.2))), rgb);
	}

	if (environment)
	{
		/* the lightmap pass's light, before the textures multiply it */
		unsigned int surface_normal = ext1(&m, GLSL_NORMALIZE, m.t_v3, xyz(&m, pixel_input(&m, &state, state.in_world_normal, m.t_v4)));
		unsigned int bump, n, sum, lit, linear_variable;
		static const unsigned int rgb[4] = { 4, 5, 6, 3 };

		if (program->stages[0].sampler == _xgpu_sampler_2d)
			bump = fsub(&m, 3, fscale(&m, 3, dynamic_light_bump, constant_float(&m, 2.0f)), constant_splat(&m, 3, 1.0f));
		else
			bump = constant_vector(&m, 3, (const float[]){ 0.0f, 0.0f, 1.0f });
		n = ext1(&m, GLSL_NORMALIZE, m.t_v3, fadd(&m, 3, fadd(&m, 3,
			fscale(&m, 3, pixel_input(&m, &state, state.in_world_tangent, m.t_v3), extract(&m, bump, 0)),
			fscale(&m, 3, pixel_input(&m, &state, state.in_world_binormal, m.t_v3), extract(&m, bump, 1))),
			fscale(&m, 3, surface_normal, fmax_scalar(&m, 1, extract(&m, bump, 2), 0.05f))));
		lt_shaded_normal = n;
		/* (the classic light model without the ray-traced lighting: lt_light_model) */
		if (!lt_light_model())
		{
			sum = dynamic_light_sum_classic(&m, &state, pixel_input(&m, &state, state.in_world_position, m.t_v3), n,
				surface_normal, TRUE, 0, 0, 0);
			lit = dynamic_light_add_classic(&m, &state, xyz(&m, result), fmul(&m, 3, sum,
				xyz(&m, uniform(&m, U_DYNAMIC_LIGHT_TINT, -1))), (program->hdr & XGPU_HDR_LIGHTS) != 0);
		}
		else
		{
		linear_variable = local(&m, &state.locals, m.t_v3);
		/* (linear_variable as the models': the linear light past the sum) */
		sum = dynamic_light_sum(&m, &state, pixel_input(&m, &state, state.in_world_position, m.t_v3), n, surface_normal, TRUE,
			0, 0, 0, linear_variable);
		/* display.rt_bounce: the bounced light, linear, into the linear sum
		(HALO_LT_E1: the traced flashlight there, added after, linearly) */
		if (!lt_e1_on())
			store(&m, linear_variable, rt_bounce_add(&m, &state, load(&m, m.t_v3, linear_variable)));
		/* (by the shader's material colour, as the gel pass multiplies it) */
		sum = fmul(&m, 3, sum, xyz(&m, uniform(&m, U_DYNAMIC_LIGHT_TINT, -1)));
		lit = dynamic_light_add(&m, &state, xyz(&m, result), sum, fmul(&m, 3, load(&m, m.t_v3, linear_variable),
			xyz(&m, uniform(&m, U_DYNAMIC_LIGHT_TINT, -1))), (program->hdr & XGPU_HDR_LIGHTS) != 0);
		lit = lt_e1_add(&m, &state, lit, ext2(&m, GLSL_POW, m.t_v3, fmax_scalar(&m, 3,
			xyz(&m, uniform(&m, U_DYNAMIC_LIGHT_TINT, -1)), 0.0f), constant_splat(&m, 3, 2.2f)),
			pixel_input(&m, &state, state.in_world_position, m.t_v3), n);
		/* (a forward receiver: the pass's light found here) */
		lit = lt_forward_add(&m, &state, lit, ext2(&m, GLSL_POW, m.t_v3, fmax_scalar(&m, 3,
			xyz(&m, uniform(&m, U_DYNAMIC_LIGHT_TINT, -1)), 0.0f), constant_splat(&m, 3, 2.2f)),
			pixel_input(&m, &state, state.in_world_position, m.t_v3), surface_normal, n);
		}
		/* (HALO_LIGHT_SHADOWS_VIEW: the lights over the surface darkened,
		not under the shoulder, so a lit wall shows them too) */
		if (state.light_shadows && light_shadows_view())
			lit = fadd(&m, 3, fscale(&m, 3, saturate(&m, 3, xyz(&m, result)), constant_float(&m, 0.25f)), sum);
		result = shuffle(&m, 4, result, lit, rgb);
	}
	if (program->coverage_alpha)
	{
		result = op3(&m, OP_COMPOSITE_INSERT, m.t_v4, ext3(&m, GLSL_F_MIX, m.t_float, constant_float(&m, 1.0f),
			extract(&m, result, 3), extract(&m, state.t[0], 1)), result, 3);
	}
	if (program->alpha_test)
	{
		unsigned int alpha = saturate(&m, 1, extract(&m, result, 3));

		if (program->alpha_test == D3DCMP_NEVER)
		{
			discard_if(&m, constant_bool(&m, TRUE));
		}
		else if (sample_mask)
		{
			/* multisampled: the samples covered as alpha passes the reference
			over the pixel */
			unsigned int test_alpha = fmul(&m, 1, alpha, constant_float(&m, 255.0f));
			unsigned int difference = fsub(&m, 1, test_alpha, uniform(&m, U_ALPHA_REFERENCE, -1));
			unsigned int coverage, samples, operands[2];

			if (comparison == OP_F_ORD_LESS_THAN || comparison == OP_F_ORD_LESS_THAN_EQUAL)
				difference = fneg(&m, 1, difference);
			coverage = saturate(&m, 1, fadd(&m, 1, fdiv(&m, 1, difference,
				fmax_scalar(&m, 1, op1(&m, OP_FWIDTH, m.t_float, test_alpha), 1.0f)), constant_float(&m, 0.5f)));
			samples = op1(&m, OP_CONVERT_F_TO_S, m.t_int, fadd(&m, 1, fmul(&m, 1, coverage,
				constant_float(&m, (float)program->alpha_test_samples)), constant_float(&m, 0.5f)));
			discard_if(&m, op2(&m, OP_I_EQUAL, m.t_bool, samples, constant_int(&m, 0)));
			operands[0] = sample_mask_out;
			operands[1] = constant_int(&m, 0);
			store(&m, op(&m, OP_ACCESS_CHAIN, type_pointer(&m, STORAGE_OUTPUT, m.t_int), 2, operands),
				op2(&m, OP_I_SUB, m.t_int, op2(&m, OP_SHIFT_LEFT_LOGICAL, m.t_int, constant_int(&m, 1), samples),
				constant_int(&m, 1)));
		}
		else if (comparison)
		{
			/* if (!(floor(clamp(result.a, 0.0, 1.0) * 255.0 + 0.5) OP alpha_reference)) discard; */
			unsigned int value = ext1(&m, GLSL_FLOOR, m.t_float, fadd(&m, 1, fmul(&m, 1, alpha, constant_float(&m, 255.0f)),
				constant_float(&m, 0.5f)));

			discard_if(&m, op1(&m, OP_LOGICAL_NOT, m.t_bool, compare(&m, comparison, value,
				uniform(&m, U_ALPHA_REFERENCE, -1))));
		}
	}
	if (options->debug_texture0)
		result = vec4_of(&m, xyz(&m, state.t[0]), constant_float(&m, 1.0f));
	if (options->debug_flat)
	{
		unsigned int d0 = pixel_input(&m, &state, state.in_d0, m.t_v4);

		result = select_n(&m, 4, compare(&m, OP_F_ORD_GREATER_THAN, extract(&m, d0, 3), constant_float(&m, 0.0f)),
			vec4_of(&m, xyz(&m, d0), constant_float(&m, 1.0f)), constant_v4(&m, 1.0f, 0.0f, 1.0f, 1.0f));
	}
	/* (debug.world_ao_view: the occlusion found in the world, in grey) */
	if (world && (program->ambient_occlusion & XGPU_AMBIENT_OCCLUSION_WORLD_VIEW))
		result = vec4_of(&m, splat(&m, 3, state.occlusion), constant_float(&m, 1.0f));
	if (count_samples)
	{
		/* samples that pass the depth and stencil tests, as the NV2A's
		occlusion counter, where the backend counts in the shader */
		unsigned int operands[4], pointer;

		operands[0] = m.samples;
		operands[1] = constant_int(&m, 0);
		operands[2] = op1(&m, OP_CONVERT_F_TO_S, m.t_int, uniform(&m, U_VISIBILITY_COUNTER, -1));
		pointer = op(&m, OP_ACCESS_CHAIN, type_pointer(&m, STORAGE_STORAGE_BUFFER, m.t_uint), 3, operands);
		operands[0] = pointer;
		operands[1] = constant_uint(&m, SCOPE_DEVICE);
		operands[2] = constant_uint(&m, 0);
		operands[3] = constant_uint(&m, 1);
		if (program->multiview == XGPU_MULTIVIEW_EYES && options->foveated)
		{
			unsigned int inset = op2(&m, OP_BITWISE_AND, m.t_int,
				view_number(&m, program->multiview, TRUE), constant_int(&m, 1));

			/* Count the whole eyes alone, as an unfoveated visibility test. */
			operands[3] = op3(&m, OP_SELECT, m.t_uint,
				compare(&m, OP_I_EQUAL, inset, constant_int(&m, 0)), constant_uint(&m, 1), constant_uint(&m, 0));
		}
		op(&m, OP_ATOMIC_I_ADD, m.t_uint, 4, operands);
	}
	/* (display.hdr: the dynamic lights' light past white, to their white
	in the display's gamma: xgpu_hdr.h) */
	if (!program->source_blend && (program->hdr & XGPU_HDR_LIGHTS))
		result = vec4_of(&m, fclamp(&m, 3, xyz(&m, result), 0.0f, XGPU_HDR_LIGHTS_WHITE_VALUE),
			saturate(&m, 1, extract(&m, result, 3)));
	/* (a screen effect's pass, the view's light past white kept) */
	else if (!program->source_blend && XGPU_HDR_IS_UNCLAMPED(program->hdr))
		result = vec4_of(&m, fmax_scalar(&m, 3, xyz(&m, result), 0.0f), saturate(&m, 1, extract(&m, result, 3)));
	else
		result = saturate(&m, 4, result);
	/* (a self-illuminated surface's light past white) */
	if (self_illuminated)
		result = vec4_of(&m, fadd(&m, 3, xyz(&m, result), self_illumination), extract(&m, result, 3));
	if (effect)
		result = effect_fade(&m, options->effect_fade, result, effect_fade_value);
	/* (an emitter's light gained by display.hdr_highlight_boost's level,
	before the fixed function adds it: xgpu_hdr_boost_emitter) */
	if (!program->source_blend && XGPU_HDR_IS_EMITTER(program->hdr))
	{
		unsigned int rgb = xyz(&m, result);
		int bound = XGPU_HDR_EMITTER_BOUND(program->hdr);

		result = vec4_of(&m, fadd(&m, 3, rgb, boost_extra(&m, rgb, rgb, XGPU_HDR_BOOST_OF(program->hdr),
			bound == XGPU_HDR_CEILING_WEAPON ? XGPU_HDR_CEILING_WEAPON_VALUE : XGPU_HDR_CEILING_LIGHT_VALUE)),
			extract(&m, result, 3));
	}
	if (program->source_blend)
	{
		unsigned int blend = uniform(&m, U_BLEND_CONSTANT, -1), factor;

		switch ((program->source_blend - 1) & 3)
		{
		case 0: factor = blend; break;
		case 1: factor = fsub(&m, 4, constant_splat(&m, 4, 1.0f), blend); break;
		case 2: factor = splat(&m, 4, extract(&m, blend, 3)); break;
		default: factor = splat(&m, 4, fsub(&m, 1, constant_float(&m, 1.0f), extract(&m, blend, 3))); break;
		}
		result = fmul(&m, 4, result, factor);
	}
	else if (XGPU_HDR_SHADER_BLENDS(program->hdr))
	{
		/* (no discard between the interlock's begin and end) */
		if (zsprite_hidden && options->interlock)
		{
			discard_if(&m, zsprite_hidden);
			zsprite_hidden = 0;
		}
		interlocked = options->interlock != 0;
		result = hdr_output(&m, program->hdr, result, options, &state.frag_coord, &target, &drawn_over);
	}
	/* a z-sprite's puff behind the scene (as an effect, tested by its fade:
	effect_zsprite_fade) */
	if (zsprite_hidden)
		discard_if(&m, zsprite_hidden);
	/* (finite: finite4; the pre-pass's twin, its inputs) */
	/* (HALO_LT_E1: the twin's guides, xgpu_spirv.h's C1) */
	if (lt_guides_out)
		store(&m, lt_guides_out, lt_e1_guides(&m, &state, lt_shaded_normal,
			program->stages[3].mode == XGPU_TEXTURE_CUBEMAP, lt_motion_current ?
			lt_guides_motion(&m, lt_motion_current, lt_motion_previous) : constant_uint(&m, XGPU_SPIRV_GUIDES_MOTION_NONE)));
	if (world_inputs)
		store(&m, color_out, world_ao_words);
	else if (occluder)
		store(&m, color_out, op(&m, OP_COMPOSITE_CONSTRUCT, m.t_uv4, 4, (unsigned int[]){ constant_uint(&m, 0),
			constant_uint(&m, 0), constant_uint(&m, 0), constant_uint(&m, 0) }));
	else
		store(&m, color_out, finite4(&m, result));
	if (motion_out)
	{
		static const float half_down[2] = { 0.5f, -0.5f };
		unsigned int current = load(&m, m.t_v3, in_motion_current), previous = load(&m, m.t_v3, in_motion_previous);
		unsigned int current_w = extract(&m, current, 2), previous_w = extract(&m, previous, 2);
		unsigned int now = fdiv(&m, 2, xy(&m, current), splat(&m, 2, current_w));
		unsigned int before = fdiv(&m, 2, xy(&m, previous), splat(&m, 2, previous_w));
		unsigned int moved, depth, both;

		moved = fmul(&m, 2, fsub(&m, 2, before, now), constant_vector(&m, 2, half_down));
		/* (none where either is behind the eye) */
		both = op2(&m, OP_LOGICAL_AND, m.t_bool,
			compare(&m, OP_F_ORD_GREATER_THAN, current_w, constant_float(&m, 0.0f)),
			compare(&m, OP_F_ORD_GREATER_THAN, previous_w, constant_float(&m, 0.0f)));
		moved = select_n(&m, 2, both, moved, constant_splat(&m, 2, 0.0f));
		depth = extract(&m, load(&m, m.t_v4, state.frag_coord), 2);
		store(&m, motion_out, construct4(&m, extract(&m, moved, 0), extract(&m, moved, 1), depth,
			constant_float(&m, 1.0f)));
	}
	if (interlocked)
	{
		unsigned int operands[2];

		hdr_store(&m, options, &target, drawn_over, finite4(&m, result));
		/* (the depth and stencil tests before the shader: the store is the
		draw's colour, and a pixel that fails them must not be written) */
		if (!count_samples)
		{
			operands[0] = main_id;
			operands[1] = EXECUTION_MODE_EARLY_FRAGMENT_TESTS;
			instruction(&m, SECTION_EXECUTION_MODES, OP_EXECUTION_MODE, 2, operands);
		}
		operands[0] = options->interlock == XGPU_SPIRV_INTERLOCK_SAMPLE ?
			CAPABILITY_FRAGMENT_SHADER_SAMPLE_INTERLOCK : CAPABILITY_FRAGMENT_SHADER_PIXEL_INTERLOCK;
		instruction(&m, SECTION_CAPABILITIES, OP_CAPABILITY, 1, operands);
		instruction_string(&m, SECTION_EXTENSIONS, OP_EXTENSION, 0, NULL, "SPV_EXT_fragment_shader_interlock", 0,
			NULL);
		operands[0] = main_id;
		operands[1] = options->interlock == XGPU_SPIRV_INTERLOCK_SAMPLE ? EXECUTION_MODE_SAMPLE_INTERLOCK_ORDERED :
			EXECUTION_MODE_PIXEL_INTERLOCK_ORDERED;
		instruction(&m, SECTION_EXECUTION_MODES, OP_EXECUTION_MODE, 2, operands);
	}
	/* else its depth, no farther than the card: the conservative depth's
	promise, so that the depth test before the shader keeps what it can (on
	reversed depth, 1 - it and no nearer than the card's reversed) */
	if (frag_depth_out && state.replaced_depth)
	{
		unsigned int operands[2], card = extract(&m, load(&m, m.t_v4, state.frag_coord), 2);

		store(&m, frag_depth_out, options->reversed_depth ?
			ext2(&m, GLSL_F_MAX, m.t_float, fsub(&m, 1, constant_float(&m, 1.0f), state.replaced_depth), card) :
			ext2(&m, GLSL_F_MIN, m.t_float, state.replaced_depth, card));
		operands[0] = main_id;
		operands[1] = EXECUTION_MODE_DEPTH_REPLACING;
		instruction(&m, SECTION_EXECUTION_MODES, OP_EXECUTION_MODE, 2, operands);
		operands[1] = options->reversed_depth ? EXECUTION_MODE_DEPTH_GREATER : EXECUTION_MODE_DEPTH_LESS;
		instruction(&m, SECTION_EXECUTION_MODES, OP_EXECUTION_MODE, 2, operands);
	}
	if (count_samples)
	{
		unsigned int operands[2];

		operands[0] = main_id;
		operands[1] = EXECUTION_MODE_EARLY_FRAGMENT_TESTS;
		instruction(&m, SECTION_EXECUTION_MODES, OP_EXECUTION_MODE, 2, operands);
	}
	if (!module_end(&m, EXECUTION_MODEL_FRAGMENT, main_id, &state.locals, module))
	{
		free(state.locals.words);
		return FALSE;
	}
	free(state.locals.words);
	return TRUE;
}

/* ---------- display.ambient_occlusion "world" found once a pixel (xgpu_spirv.h)

The passes after the pre-pass: xgpu_msl.c's xgpu_msl_world_ao_pass,
operation for operation; world_ao_shading.c's world_ao_pass_position,
world_ao_upsample, world_ao_inputs_unpack and world_ao_pass_occlusion,
their CPU reference. */

struct world_ao_pass_state
{
	unsigned int pass, inputs, frag_coord, history, cascades;
	/* with "ray_traced"'s ray queries (xgpu_spirv_options ray_queries): the
	ray-traced scene's top-level acceleration structure (0 none) */
	unsigned int scene;
};

/* (struct xgpu_world_ao_pass's members: the camera, its corners, the
cascades, the previous camera and corners, the history's) */
enum
{
	WORLD_AO_PASS_CAMERA, WORLD_AO_PASS_CORNERS, WORLD_AO_PASS_CASCADES, WORLD_AO_PASS_PREVIOUS,
	WORLD_AO_PASS_PREVIOUS_CORNERS, WORLD_AO_PASS_HISTORY,
};

static void world_ao_pass_declare(struct module *m, struct world_ao_pass_state *pass, BOOL rays)
{
	unsigned int members[6], block, unsigned_image = type_sampled_image(m, DIM_2D, FALSE, TRUE, TRUE);

	members[0] = block_array(m, 4);
	members[1] = m->t_iv4;
	members[2] = block_array(m, XGPU_WORLD_CASCADES);
	members[3] = members[0];
	members[4] = m->t_iv4;
	members[5] = m->t_v4;
	block = type_struct(m, 6, members);
	decorate(m, block, DECORATION_BLOCK, 0, NULL);
	member_decorate(m, block, 0, DECORATION_OFFSET, 0);
	member_decorate(m, block, 1, DECORATION_OFFSET, 64);
	member_decorate(m, block, 2, DECORATION_OFFSET, 80);
	member_decorate(m, block, 3, DECORATION_OFFSET, 80 + 16 * XGPU_WORLD_CASCADES);
	member_decorate(m, block, 4, DECORATION_OFFSET, 144 + 16 * XGPU_WORLD_CASCADES);
	member_decorate(m, block, 5, DECORATION_OFFSET, 160 + 16 * XGPU_WORLD_CASCADES);
	name(m, block, "world_ao_pass_block");
	pass->pass = variable(m, block, STORAGE_UNIFORM, "world_ao_pass");
	decorate1(m, pass->pass, DECORATION_DESCRIPTOR_SET, XGPU_SPIRV_SET_DRAW);
	decorate1(m, pass->pass, DECORATION_BINDING, XGPU_SPIRV_BINDING_UNIFORMS);
	pass->inputs = variable(m, unsigned_image, STORAGE_UNIFORM_CONSTANT, "world_ao_inputs");
	decorate1(m, pass->inputs, DECORATION_DESCRIPTOR_SET, XGPU_SPIRV_SET_TEXTURES);
	decorate1(m, pass->inputs, DECORATION_BINDING, XGPU_SPIRV_WORLD_AO_PASS_INPUTS);
	pass->history = variable(m, type_sampled_image(m, DIM_2D, FALSE, FALSE, TRUE), STORAGE_UNIFORM_CONSTANT,
		"world_ao_history");
	decorate1(m, pass->history, DECORATION_DESCRIPTOR_SET, XGPU_SPIRV_SET_TEXTURES);
	decorate1(m, pass->history, DECORATION_BINDING, XGPU_SPIRV_WORLD_AO_PASS_HISTORY);
	pass->cascades = variable(m, type_sampled_image(m, DIM_3D, FALSE, FALSE, TRUE), STORAGE_UNIFORM_CONSTANT,
		"world_cascades");
	decorate1(m, pass->cascades, DECORATION_DESCRIPTOR_SET, XGPU_SPIRV_SET_TEXTURES);
	decorate1(m, pass->cascades, DECORATION_BINDING, XGPU_SPIRV_WORLD_AO_PASS_CASCADES);
	pass->frag_coord = variable(m, m->t_v4, STORAGE_INPUT, "gl_FragCoord");
	decorate1(m, pass->frag_coord, DECORATION_BUILT_IN, BUILT_IN_FRAG_COORD);
	pass->scene = 0;
	if (rays)
	{
		pass->scene = variable(m, type0(m, OP_TYPE_ACCELERATION_STRUCTURE), STORAGE_UNIFORM_CONSTANT, "world_ao_scene");
		decorate1(m, pass->scene, DECORATION_DESCRIPTOR_SET, XGPU_SPIRV_SET_OCCLUSION);
		decorate1(m, pass->scene, DECORATION_BINDING, XGPU_SPIRV_BINDING_WORLD_AO_SCENE);
		m->ray_query = TRUE;
	}
}

static unsigned int world_ao_pass_vector(struct module *m, struct world_ao_pass_state *pass, int member, int index)
{
	int whole = member == WORLD_AO_PASS_CORNERS || member == WORLD_AO_PASS_PREVIOUS_CORNERS ||
		member == WORLD_AO_PASS_HISTORY;
	unsigned int operands[3], type = member == WORLD_AO_PASS_CORNERS || member == WORLD_AO_PASS_PREVIOUS_CORNERS ?
		m->t_iv4 : m->t_v4;

	operands[0] = pass->pass;
	operands[1] = constant_int(m, member);
	operands[2] = constant_int(m, index);
	return load(m, type, op(m, OP_ACCESS_CHAIN, type_pointer(m, STORAGE_UNIFORM, type), whole ? 2 : 3, operands));
}

/* texelFetch(image, at, 0) of an unsigned image variable (a uvec4) */
static unsigned int world_ao_pass_fetch(struct module *m, unsigned int image_variable, unsigned int at)
{
	unsigned int operands[4], image = load(m, type_sampled_image(m, DIM_2D, FALSE, TRUE, TRUE), image_variable);

	operands[0] = op(m, OP_IMAGE, type_sampled_image(m, DIM_2D, FALSE, TRUE, FALSE), 1, &image);
	operands[1] = at;
	operands[2] = IMAGE_OPERANDS_LOD;
	operands[3] = constant_int(m, 0);
	return op(m, OP_IMAGE_FETCH, m->t_uv4, 4, operands);
}

/* a pixel's world position: its distance from the eye (the block's
grid[0]) along the ray through its centre (world_ao_pass_position) */
static unsigned int world_ao_pass_position(struct module *m, struct world_ao_pass_state *pass, unsigned int pixel,
	unsigned int distance)
{
	unsigned int camera[4], corners = world_ao_pass_vector(m, pass, 1, 0), low, size, screen, ray;
	int index;

	for (index = 1; index < 4; index++)
		camera[index] = world_ao_pass_vector(m, pass, 0, index);
	low = op1(m, OP_CONVERT_S_TO_F, m->t_v2, op(m, OP_VECTOR_SHUFFLE, m->t_iv2, 4,
		(unsigned int[]){ corners, corners, 0, 1 }));
	size = fsub(m, 2, op1(m, OP_CONVERT_S_TO_F, m->t_v2, op(m, OP_VECTOR_SHUFFLE, m->t_iv2, 4,
		(unsigned int[]){ corners, corners, 2, 3 })), low);
	screen = fsub(m, 2, fdiv(m, 2, fscale(m, 2, fsub(m, 2, fadd(m, 2, op1(m, OP_CONVERT_S_TO_F, m->t_v2, pixel),
		constant_splat(m, 2, 0.5f)), low), constant_float(m, 2.0f)), size), constant_splat(m, 2, 1.0f));
	ray = fadd(m, 3, fadd(m, 3, xyz(m, camera[1]), fscale(m, 3, xyz(m, camera[2]), extract(m, screen, 0))),
		fscale(m, 3, xyz(m, camera[3]), extract(m, screen, 1)));
	return fadd(m, 3, xyz(m, world_ao_vector(m, 3, constant_int(m, 0))),
		fscale(m, 3, ext1(m, GLSL_NORMALIZE, m->t_v3, ray), distance));
}

static unsigned int uand(struct module *m, unsigned int a, unsigned int b)
{
	return op2(m, OP_BITWISE_AND, m->t_uint, a, b);
}

static unsigned int unot_zero(struct module *m, unsigned int a)
{
	return op2(m, OP_I_NOT_EQUAL, m->t_bool, a, constant_uint(m, 0));
}

/* a pixel inside the window's corners */
static unsigned int world_ao_pass_inside(struct module *m, unsigned int pixel, unsigned int corners)
{
	return op2(m, OP_LOGICAL_AND, m->t_bool, op2(m, OP_LOGICAL_AND, m->t_bool,
		op2(m, OP_S_LESS_THAN, m->t_bool, op2(m, OP_COMPOSITE_EXTRACT, m->t_int, pixel, 0), op2(m, OP_COMPOSITE_EXTRACT, m->t_int, corners, 2)),
		op2(m, OP_S_LESS_THAN, m->t_bool, op2(m, OP_COMPOSITE_EXTRACT, m->t_int, pixel, 1), op2(m, OP_COMPOSITE_EXTRACT, m->t_int, corners, 3))),
		op1(m, OP_LOGICAL_NOT, m->t_bool, op2(m, OP_LOGICAL_OR, m->t_bool,
		op2(m, OP_S_LESS_THAN, m->t_bool, op2(m, OP_COMPOSITE_EXTRACT, m->t_int, pixel, 0), op2(m, OP_COMPOSITE_EXTRACT, m->t_int, corners, 0)),
		op2(m, OP_S_LESS_THAN, m->t_bool, op2(m, OP_COMPOSITE_EXTRACT, m->t_int, pixel, 1), op2(m, OP_COMPOSITE_EXTRACT, m->t_int, corners, 1)))));
}

/* ---------- the ray-traced scene's alpha-tested candidates
(docs/rt-plan/1-geometry.md, stage 3; xgpu_vulkan_rt.c's hit table)

A ray query's candidate in an instance's alpha-tested geometry (geometry
1: a grate, a fence, a foliage card) is taken only where its material's
alpha map passes the cutoff at the hit: the instance's mesh (its shader
binding table offset), the mesh's record, the triangle's corners and their
texture coordinates, its material's record, its alpha map sampled at a
fixed level (no derivatives in the loop), as GLSL:

bool passes = true;
if (rayQueryGetIntersectionGeometryIndexEXT(q, false) == 1) {
  uint mesh = rayQueryGetIntersectionInstanceShaderBindingTableRecordOffsetEXT(q, false) & 0xFFF;
  uint prim = rayQueryGetIntersectionPrimitiveIndexEXT(q, false) + table[mesh * 4 + 3];
  uint corners = table[mesh * 4] + prim * 3, attributes = table[mesh * 4 + 1];
  uint material = (table[table[mesh * 4 + 2] + prim / 2] >> (prim % 2 * 16)) & 0xFFFF, at = MATERIALS + material * 16;
  vec2 uv[3] = each corner's uintBitsToFloat(table[attributes + corner * 6 ..+1]);
  vec2 b = rayQueryGetIntersectionBarycentricsEXT(q, false);
  vec2 st = (uv[0] + (uv[1] - uv[0]) * b.x + (uv[2] - uv[0]) * b.y) * uintBitsToFloat(table[at + 5 ..+6]);
  passes = textureLod(textures[nonuniformEXT(min(table[at + 4], TEXTURES - 1))], st, LOD).a > uintBitsToFloat(table[at + 3]);
}
commits = (condition || structure) && passes;

Each word read is in range: the backend checked the meshes and materials
as they came. No loop of its own: it is a candidate loop's body. */

/* the hit set's variables, declared once in a module */
static void rt_hits_declare(struct module *m)
{
	unsigned int array, block, operands[3], image;

	if (m->rt_hit_table)
		return;
	/* (a runtime array of its own: its stride decorated once) */
	array = id_new(m);
	operands[0] = array;
	operands[1] = m->t_uint;
	instruction(m, SECTION_GLOBALS, OP_TYPE_RUNTIME_ARRAY, 2, operands);
	decorate1(m, array, DECORATION_ARRAY_STRIDE, 4);
	block = type_struct(m, 1, &array);
	decorate(m, block, DECORATION_BLOCK, 0, NULL);
	member_decorate(m, block, 0, DECORATION_OFFSET, 0);
	operands[0] = block;
	operands[1] = 0;
	operands[2] = DECORATION_NON_WRITABLE;
	instruction(m, SECTION_DECORATIONS, OP_MEMBER_DECORATE, 3, operands);
	name(m, block, "rt_hit_table_block");
	m->rt_hit_table = variable(m, block, STORAGE_STORAGE_BUFFER, "rt_hit_table");
	decorate1(m, m->rt_hit_table, DECORATION_DESCRIPTOR_SET, XGPU_SPIRV_SET_RT_HITS);
	decorate1(m, m->rt_hit_table, DECORATION_BINDING, XGPU_SPIRV_BINDING_RT_HIT_TABLE);
	image = type_sampled_image(m, DIM_2D, FALSE, FALSE, TRUE);
	m->rt_hit_textures = variable(m, type2(m, OP_TYPE_ARRAY, image, constant_uint(m, XGPU_SPIRV_RT_HIT_TEXTURES)),
		STORAGE_UNIFORM_CONSTANT, "rt_hit_textures");
	decorate1(m, m->rt_hit_textures, DECORATION_DESCRIPTOR_SET, XGPU_SPIRV_SET_RT_HITS);
	decorate1(m, m->rt_hit_textures, DECORATION_BINDING, XGPU_SPIRV_BINDING_RT_HIT_TEXTURES);
}

/* the hit table's word at (a uint) */
static unsigned int rt_hit_word(struct module *m, unsigned int at)
{
	unsigned int operands[3];

	operands[0] = m->rt_hit_table;
	operands[1] = constant_int(m, 0);
	operands[2] = at;
	return load(m, m->t_uint, op(m, OP_ACCESS_CHAIN, type_pointer(m, STORAGE_STORAGE_BUFFER, m->t_uint), 3, operands));
}

static unsigned int rt_hit_float(struct module *m, unsigned int at)
{
	return op1(m, OP_BITCAST, m->t_float, rt_hit_word(m, at));
}

static unsigned int uadd(struct module *m, unsigned int a, unsigned int b)
{
	return op2(m, OP_I_ADD, m->t_uint, a, b);
}

/* whether a ray query's candidate is taken: condition (the caller's: not
the pixel's own, or the caster's) or the structure's (structure: its
custom index 0, which no owner skips), where its alpha map passes; the
candidate loop's body */
static unsigned int rt_candidate_commits(struct module *m, struct words *locals, unsigned int query,
	unsigned int structure, unsigned int condition)
{
	unsigned int candidate = constant_int(m, RAY_QUERY_CANDIDATE), passes = local(m, locals, m->t_float), merge;
	unsigned int mesh, record, primitive, corners, attributes, material_word, material, at, uv[3], b, st, slot;
	unsigned int operands[4], pointer, sampled, texel;
	int corner;

	rt_hits_declare(m);
	store(m, passes, constant_float(m, 1.0f));
	selection_begin(m, op2(m, OP_I_EQUAL, m->t_bool, op2(m, OP_RAY_QUERY_GET_INTERSECTION_GEOMETRY_INDEX, m->t_uint, query,
		candidate), constant_uint(m, 1)), &merge);
	mesh = uand(m, op2(m, OP_RAY_QUERY_GET_INTERSECTION_INSTANCE_SHADER_BINDING_TABLE_RECORD_OFFSET, m->t_uint, query,
		candidate), constant_uint(m, XGPU_SPIRV_RT_HIT_MESHES - 1));
	record = op2(m, OP_SHIFT_LEFT_LOGICAL, m->t_uint, mesh, constant_uint(m, 2));
	primitive = uadd(m, op2(m, OP_RAY_QUERY_GET_INTERSECTION_PRIMITIVE_INDEX, m->t_uint, query, candidate),
		rt_hit_word(m, uadd(m, record, constant_uint(m, 3))));
	corners = uadd(m, rt_hit_word(m, record), op2(m, OP_I_MUL, m->t_uint, primitive, constant_uint(m, 3)));
	attributes = rt_hit_word(m, uadd(m, record, constant_uint(m, 1)));
	material_word = rt_hit_word(m, uadd(m, rt_hit_word(m, uadd(m, record, constant_uint(m, 2))),
		op2(m, OP_SHIFT_RIGHT_LOGICAL, m->t_uint, primitive, constant_uint(m, 1))));
	material = uand(m, op2(m, OP_SHIFT_RIGHT_LOGICAL, m->t_uint, material_word, op2(m, OP_SHIFT_LEFT_LOGICAL, m->t_uint,
		uand(m, primitive, constant_uint(m, 1)), constant_uint(m, 4))), constant_uint(m, 0xFFFFu));
	at = uadd(m, constant_uint(m, XGPU_SPIRV_RT_HIT_MATERIALS), op2(m, OP_SHIFT_LEFT_LOGICAL, m->t_uint, material,
		constant_uint(m, 4)));
	for (corner = 0; corner < 3; corner++)
	{
		unsigned int vertex = rt_hit_word(m, uadd(m, corners, constant_uint(m, (unsigned int)corner)));
		unsigned int first = uadd(m, attributes, op2(m, OP_I_MUL, m->t_uint, vertex, constant_uint(m, 6))), xy[2];

		xy[0] = rt_hit_float(m, first);
		xy[1] = rt_hit_float(m, uadd(m, first, constant_uint(m, 1)));
		uv[corner] = construct(m, 2, xy);
	}
	b = op2(m, OP_RAY_QUERY_GET_INTERSECTION_BARYCENTRICS, m->t_v2, query, candidate);
	st = fadd(m, 2, fadd(m, 2, uv[0], fscale(m, 2, fsub(m, 2, uv[1], uv[0]), extract(m, b, 0))),
		fscale(m, 2, fsub(m, 2, uv[2], uv[0]), extract(m, b, 1)));
	{
		unsigned int scale[2];

		scale[0] = rt_hit_float(m, uadd(m, at, constant_uint(m, 5)));
		scale[1] = rt_hit_float(m, uadd(m, at, constant_uint(m, 6)));
		st = fmul(m, 2, st, construct(m, 2, scale));
	}
	slot = ext2(m, GLSL_U_MIN, m->t_uint, rt_hit_word(m, uadd(m, at, constant_uint(m, 4))),
		constant_uint(m, XGPU_SPIRV_RT_HIT_TEXTURES - 1));
	decorate(m, slot, DECORATION_NON_UNIFORM, 0, NULL);
	operands[0] = m->rt_hit_textures;
	operands[1] = slot;
	pointer = op(m, OP_ACCESS_CHAIN, type_pointer(m, STORAGE_UNIFORM_CONSTANT, type_sampled_image(m, DIM_2D, FALSE, FALSE,
		TRUE)), 2, operands);
	decorate(m, pointer, DECORATION_NON_UNIFORM, 0, NULL);
	sampled = load(m, type_sampled_image(m, DIM_2D, FALSE, FALSE, TRUE), pointer);
	decorate(m, sampled, DECORATION_NON_UNIFORM, 0, NULL);
	operands[0] = sampled;
	operands[1] = st;
	operands[2] = IMAGE_OPERANDS_LOD;
	operands[3] = constant_float(m, XGPU_SPIRV_RT_HIT_LEVEL);
	texel = op(m, OP_IMAGE_SAMPLE_EXPLICIT_LOD, m->t_v4, 4, operands);
	store(m, passes, fselect(m, compare(m, OP_F_ORD_GREATER_THAN, extract(m, texel, 3), rt_hit_float(m, uadd(m, at,
		constant_uint(m, 3)))), constant_float(m, 1.0f), constant_float(m, 0.0f)));
	selection_end(m, merge);
	return op2(m, OP_LOGICAL_AND, m->t_bool, op2(m, OP_LOGICAL_OR, m->t_bool, condition, structure),
		compare(m, OP_F_ORD_GREATER_THAN, load(m, m->t_float, passes), constant_float(m, 0.5f)));
}

/* ---------- display.rt_reflections (docs/rt-plan/2-reflections.md)

A draw the game tags (xgpu_shader.h's rt_reflection) has its reflection's
texel, texture 3's cube map lookup, traced instead: one ray from the
pixel's world position along the very vector the game looks its cube map
up by (the vertex program's, or the bump map's DOT_REFLECT_SPECULAR
reflection), so a bumped floor's ripples bend it as they bent the cube
map. The game's combiners after it (its masks, fresnel, tints, the
lightmap's reflection mask in the target's alpha) are untouched: only the
cube map's colour is the scene's. As GLSL:

vec3 d = normalize(r);
rayQueryInitializeEXT(q, scene, 0, MASK_REFLECTIONS, p + d * START, 0.0, d, DISTANCE);
while (rayQueryProceedEXT(q)) if (alpha passes: rt_candidate_commits) rayQueryConfirmIntersectionEXT(q);
vec4 texel = cube;
if (hit && its mesh has attributes) {
  the triangle's material and corners (rt_candidate_commits' reading, of the committed hit);
  vec3 c = base(uv * (model ? its map scale : 1)).rgb;
  if (environment && detail) c *= 2.0 * detail(uv * detail_scale).rgb;      (the default biased multiply)
  c *= lightmap ? lightmap(lightmap_uv).rgb : vec3(OBJECT_LIGHT);
  texel.rgb = mix(clamp(c, 0, 1), cube.rgb, smoothstep(DISTANCE * FADE, DISTANCE, t));
}
(a model's material roughness: texel = mix(texel, cube, smoothstep(ROUGH, ROUGHEST, roughness)))

A miss, a hit past the distance, or a hit without attributes (the
collision meshes, an object's box) keeps the cube map: the sky, as Halo
drew it. No second bounce; the hit's own reflection is not added. */
#define RT_REFLECTION_DISTANCE 25.0f
#define RT_REFLECTION_FADE 0.8f
#define RT_REFLECTION_START 0.02f
#define RT_REFLECTION_ROUGH 0.2f
#define RT_REFLECTION_ROUGHEST 0.35f
/* (an object hit's light: the hit table has no object's lighting yet) */
#define RT_REFLECTION_OBJECT_LIGHT 0.6f
/* (the shader types of world_rt_game.c's materials: shaders.c's) */
#define RT_REFLECTION_KIND_ENVIRONMENT 3u
#define RT_REFLECTION_KIND_MODEL 4u

/* HALO_RT_REFLECTIONS_VIEW=1: each traced texel green where its ray met
the scene with attributes, red where it kept the cube map (for checking
which surfaces trace) */
static int rt_reflections_view(void)
{
	static int view = -1;

	if (view < 0)
	{
		const char *given = getenv("HALO_RT_REFLECTIONS_VIEW");

		view = given ? atoi(given) : 0;
	}
	return view;
}

/* a texel of the hit set's textures: slot (a uint, clamped), at st, at a
fixed level */
static unsigned int rt_hit_texel(struct module *m, unsigned int slot_word, unsigned int st, float level)
{
	unsigned int slot = ext2(m, GLSL_U_MIN, m->t_uint, slot_word, constant_uint(m, XGPU_SPIRV_RT_HIT_TEXTURES - 1));
	unsigned int operands[4], pointer, sampled;

	decorate(m, slot, DECORATION_NON_UNIFORM, 0, NULL);
	operands[0] = m->rt_hit_textures;
	operands[1] = slot;
	pointer = op(m, OP_ACCESS_CHAIN, type_pointer(m, STORAGE_UNIFORM_CONSTANT, type_sampled_image(m, DIM_2D, FALSE, FALSE,
		TRUE)), 2, operands);
	decorate(m, pointer, DECORATION_NON_UNIFORM, 0, NULL);
	sampled = load(m, type_sampled_image(m, DIM_2D, FALSE, FALSE, TRUE), pointer);
	decorate(m, sampled, DECORATION_NON_UNIFORM, 0, NULL);
	operands[0] = sampled;
	operands[1] = st;
	operands[2] = IMAGE_OPERANDS_LOD;
	operands[3] = constant_float(m, level);
	return op(m, OP_IMAGE_SAMPLE_EXPLICIT_LOD, m->t_v4, 4, operands);
}

/* the one light model's reflection colour (~/dev/rt2-realtime-design.md
section 10, "Reflection colour"): a traced reflection is the scene's light,
and Halo's cube map carries, besides its fake scene, the material's own hue
(an Elite's armour's blue-violet, the Covenant's metals): its chroma, the
whole map's mean (its smallest level) over that mean's luminance, tints the
traced light as a metal's F0 colour would, the luminance left to the scene.
A grey cube map tints nothing. As GLSL:
vec3 mean = textureLod(cube, vec3(0, 0, 1), 16.0).rgb;
return clamp(mean / max(dot(mean, vec3(0.2126, 0.7152, 0.0722)), 1e-3), 0.0, 4.0) */
static unsigned int lt_reflection_chroma(struct module *m, struct pixel_state *state)
{
	unsigned int operands[4], mean;

	operands[0] = texture_load(m, state, 3);
	operands[1] = constant_vector(m, 3, (const float[]){ 0.0f, 0.0f, 1.0f });
	operands[2] = IMAGE_OPERANDS_LOD;
	operands[3] = constant_float(m, 16.0f);
	mean = xyz(m, op(m, OP_IMAGE_SAMPLE_EXPLICIT_LOD, m->t_v4, 4, operands));
	return fclamp(m, 3, fdiv(m, 3, mean, splat(m, 3, fmax_scalar(m, 1, fdot(m, mean,
		constant_vector(m, 3, (const float[]){ 0.2126f, 0.7152f, 0.0722f })), 1.0e-3f))), 0.0f, 4.0f);
}

/* a model's traced reflection with the one light model (~/dev/rt2-realtime-
design.md section 10, "Reflection colour"): the cube map's texel at the
ray's direction scaled by how bright the scene is there against the cube
map's own mean, at most 4 times, as GLSL:
vec3 mean = textureLod(cube, vec3(0, 0, 1), 16.0).rgb;
return texel * clamp(lum(traced) / max(lum(mean), 1e-3), 0.0, 4.0); */
static unsigned int lt_reflection_lit_cube(struct module *m, struct pixel_state *state, unsigned int traced,
	unsigned int texel)
{
	unsigned int operands[4], mean, weights = constant_vector(m, 3, (const float[]){ 0.2126f, 0.7152f, 0.0722f });

	operands[0] = texture_load(m, state, 3);
	operands[1] = constant_vector(m, 3, (const float[]){ 0.0f, 0.0f, 1.0f });
	operands[2] = IMAGE_OPERANDS_LOD;
	operands[3] = constant_float(m, 16.0f);
	mean = xyz(m, op(m, OP_IMAGE_SAMPLE_EXPLICIT_LOD, m->t_v4, 4, operands));
	return fscale(m, 3, texel, fclamp_scalar(m, fdiv(m, 1, fdot(m, traced, weights),
		fmax_scalar(m, 1, fdot(m, mean, weights), 1.0e-3f)), 0.0f, 4.0f));
}

static unsigned int rt_reflection_texel(struct module *m, struct pixel_state *state, unsigned int r, unsigned int cube)
{
	struct words *locals = &state->locals;
	unsigned int query = local(m, locals, type0(m, OP_TYPE_RAY_QUERY)), texel = local(m, locals, m->t_v4);
	unsigned int committed = constant_int(m, RAY_QUERY_COMMITTED), candidate = constant_int(m, RAY_QUERY_CANDIDATE);
	unsigned int p = pixel_input(m, state, state->in_world_position, m->t_v3);
	unsigned int d = ext1(m, GLSL_NORMALIZE, m->t_v3, r);
	unsigned int header = id_new(m), condition = id_new(m), body = id_new(m), next = id_new(m), merge = id_new(m);
	unsigned int operands[8], kind, hit_merge, attributes_merge, mine_merge, value, structure;
	unsigned int t, mesh, record, attributes, geometry, primitive, corners, material_word, material, at;
	unsigned int uv[3], lightmap_uv[3], b, st, lst, shader_kind, scale[2], base_scale, color, detail, light, fade;
	unsigned int ray_length;
	int corner;

	if (!m->rt_reflection_scene)
	{
		m->rt_reflection_scene = variable(m, type0(m, OP_TYPE_ACCELERATION_STRUCTURE), STORAGE_UNIFORM_CONSTANT,
			"rt_reflection_scene");
		decorate1(m, m->rt_reflection_scene, DECORATION_DESCRIPTOR_SET, XGPU_SPIRV_SET_OCCLUSION);
		decorate1(m, m->rt_reflection_scene, DECORATION_BINDING, XGPU_SPIRV_BINDING_WORLD_AO_SCENE);
		m->ray_query = TRUE;
	}
	rt_hits_declare(m);
	store(m, texel, cube);
	/* (water: a ray below the horizon keeps the cube map, its length 0) */
	ray_length = constant_float(m, RT_REFLECTION_DISTANCE);
	if (state->program->rt_reflection == XGPU_RT_REFLECTION_WATER)
		ray_length = fselect(m, compare(m, OP_F_ORD_GREATER_THAN, extract(m, d, 2), constant_float(m, 0.0f)), ray_length,
			constant_float(m, 0.0f));
	operands[0] = query;
	operands[1] = load(m, type0(m, OP_TYPE_ACCELERATION_STRUCTURE), m->rt_reflection_scene);
	operands[2] = constant_uint(m, 0);
	operands[3] = constant_uint(m, XGPU_SPIRV_RT_MASK_REFLECTIONS);
	operands[4] = fadd(m, 3, p, fscale(m, 3, d, constant_float(m, RT_REFLECTION_START)));
	operands[5] = constant_float(m, 0.0f);
	operands[6] = d;
	operands[7] = ray_length;
	op_void(m, OP_RAY_QUERY_INITIALIZE, 8, operands);
	branch(m, header);
	label_at(m, header);
	operands[0] = merge;
	operands[1] = next;
	operands[2] = 0;
	op_void(m, OP_LOOP_MERGE, 3, operands);
	branch(m, condition);
	label_at(m, condition);
	branch_conditional(m, op1(m, OP_RAY_QUERY_PROCEED, m->t_bool, query), body, merge);
	label_at(m, body);
	/* (every owner's: a surface sees itself, the player too) */
	structure = op2(m, OP_I_EQUAL, m->t_bool, op2(m, OP_RAY_QUERY_GET_INTERSECTION_INSTANCE_CUSTOM_INDEX, m->t_int, query,
		candidate), constant_int(m, 0));
	selection_begin(m, rt_candidate_commits(m, locals, query, structure, constant_bool(m, TRUE)), &mine_merge);
	op_void(m, OP_RAY_QUERY_CONFIRM_INTERSECTION, 1, &query);
	selection_end(m, mine_merge);
	branch(m, next);
	label_at(m, next);
	branch(m, header);
	label_at(m, merge);
	kind = op2(m, OP_RAY_QUERY_GET_INTERSECTION_TYPE, m->t_uint, query, committed);
	/* (the hit's mesh, and its attributes: 0 a mesh of triangles alone) */
	mesh = uand(m, op2(m, OP_RAY_QUERY_GET_INTERSECTION_INSTANCE_SHADER_BINDING_TABLE_RECORD_OFFSET, m->t_uint, query,
		committed), constant_uint(m, XGPU_SPIRV_RT_HIT_MESHES - 1));
	record = op2(m, OP_SHIFT_LEFT_LOGICAL, m->t_uint, mesh, constant_uint(m, 2));
	selection_begin(m, op2(m, OP_I_NOT_EQUAL, m->t_bool, kind, constant_uint(m, 0)), &hit_merge);
	attributes = rt_hit_word(m, uadd(m, record, constant_uint(m, 1)));
	selection_begin(m, unot_zero(m, attributes), &attributes_merge);
	t = op2(m, OP_RAY_QUERY_GET_INTERSECTION_T, m->t_float, query, committed);
	geometry = op2(m, OP_RAY_QUERY_GET_INTERSECTION_GEOMETRY_INDEX, m->t_uint, query, committed);
	/* (an alpha-tested triangle's index past the opaque ones) */
	primitive = uadd(m, op2(m, OP_RAY_QUERY_GET_INTERSECTION_PRIMITIVE_INDEX, m->t_uint, query, committed),
		op3(m, OP_SELECT, m->t_uint, op2(m, OP_I_EQUAL, m->t_bool, geometry, constant_uint(m, 1)),
		rt_hit_word(m, uadd(m, record, constant_uint(m, 3))), constant_uint(m, 0)));
	corners = uadd(m, rt_hit_word(m, record), op2(m, OP_I_MUL, m->t_uint, primitive, constant_uint(m, 3)));
	material_word = rt_hit_word(m, uadd(m, rt_hit_word(m, uadd(m, record, constant_uint(m, 2))),
		op2(m, OP_SHIFT_RIGHT_LOGICAL, m->t_uint, primitive, constant_uint(m, 1))));
	material = uand(m, op2(m, OP_SHIFT_RIGHT_LOGICAL, m->t_uint, material_word, op2(m, OP_SHIFT_LEFT_LOGICAL, m->t_uint,
		uand(m, primitive, constant_uint(m, 1)), constant_uint(m, 4))), constant_uint(m, 0xFFFFu));
	at = uadd(m, constant_uint(m, XGPU_SPIRV_RT_HIT_MATERIALS), op2(m, OP_SHIFT_LEFT_LOGICAL, m->t_uint, material,
		constant_uint(m, 4)));
	for (corner = 0; corner < 3; corner++)
	{
		unsigned int vertex = rt_hit_word(m, uadd(m, corners, constant_uint(m, (unsigned int)corner)));
		unsigned int first = uadd(m, attributes, op2(m, OP_I_MUL, m->t_uint, vertex, constant_uint(m, 6))), xy_parts[2];

		xy_parts[0] = rt_hit_float(m, first);
		xy_parts[1] = rt_hit_float(m, uadd(m, first, constant_uint(m, 1)));
		uv[corner] = construct(m, 2, xy_parts);
		lightmap_uv[corner] = ext1(m, GLSL_UNPACK_SNORM_2X16, m->t_v2, rt_hit_word(m, uadd(m, first, constant_uint(m, 4))));
	}
	b = op2(m, OP_RAY_QUERY_GET_INTERSECTION_BARYCENTRICS, m->t_v2, query, committed);
	st = fadd(m, 2, fadd(m, 2, uv[0], fscale(m, 2, fsub(m, 2, uv[1], uv[0]), extract(m, b, 0))),
		fscale(m, 2, fsub(m, 2, uv[2], uv[0]), extract(m, b, 1)));
	lst = fadd(m, 2, fadd(m, 2, lightmap_uv[0], fscale(m, 2, fsub(m, 2, lightmap_uv[1], lightmap_uv[0]), extract(m, b, 0))),
		fscale(m, 2, fsub(m, 2, lightmap_uv[2], lightmap_uv[0]), extract(m, b, 1)));
	shader_kind = rt_hit_word(m, uadd(m, at, constant_uint(m, 1)));
	/* (a model's base map at its map's scale, which the material keeps in
	the alpha map's: world_rt_game.c) */
	scale[0] = rt_hit_float(m, uadd(m, at, constant_uint(m, 5)));
	scale[1] = rt_hit_float(m, uadd(m, at, constant_uint(m, 6)));
	base_scale = select_n(m, 2, op2(m, OP_I_EQUAL, m->t_bool, shader_kind, constant_uint(m, RT_REFLECTION_KIND_MODEL)),
		construct(m, 2, scale), constant_splat(m, 2, 1.0f));
	color = xyz(m, rt_hit_texel(m, rt_hit_word(m, uadd(m, at, constant_uint(m, 7))), fmul(m, 2, st, base_scale),
		XGPU_SPIRV_RT_HIT_LEVEL));
	/* (an environment's detail map, as its default function: biased multiply) */
	{
		unsigned int detail_slot = rt_hit_word(m, uadd(m, at, constant_uint(m, 8)));
		unsigned int detail_scale = rt_hit_float(m, uadd(m, at, constant_uint(m, 9)));

		detail = fscale(m, 3, xyz(m, rt_hit_texel(m, detail_slot, fscale(m, 2, st, detail_scale), XGPU_SPIRV_RT_HIT_LEVEL)),
			constant_float(m, 2.0f));
		detail = select_n(m, 3, op2(m, OP_LOGICAL_AND, m->t_bool, unot_zero(m, detail_slot), op2(m, OP_I_EQUAL, m->t_bool,
			shader_kind, constant_uint(m, RT_REFLECTION_KIND_ENVIRONMENT))), detail, constant_splat(m, 3, 1.0f));
	}
	{
		unsigned int lightmap_slot = rt_hit_word(m, uadd(m, at, constant_uint(m, 14)));

		light = select_n(m, 3, unot_zero(m, lightmap_slot), xyz(m, rt_hit_texel(m, lightmap_slot, lst, 0.0f)),
			constant_splat(m, 3, RT_REFLECTION_OBJECT_LIGHT));
	}
	color = saturate(m, 3, fmul(m, 3, fmul(m, 3, color, detail), light));
	/* (the one light model, a model's: Halo's cube map kept as the armour's
	or paint's own pattern and hue, its brightness the scene's: the cube's
	texel by the traced light's luminance over the cube's mean's, so an
	Elite keeps its sheen and darkens where the scene is dark:
	lt_reflection_lit_cube) */
	if (lt_e1_on())
		color = lt_reflection_lit_cube(m, state, color, xyz(m, cube));
	fade = ext3(m, GLSL_SMOOTH_STEP, m->t_float, constant_float(m, RT_REFLECTION_DISTANCE * RT_REFLECTION_FADE),
		constant_float(m, RT_REFLECTION_DISTANCE), t);
	color = ext3(m, GLSL_F_MIX, m->t_v3, color, xyz(m, cube), splat(m, 3, fade));
	if (rt_reflections_view() == 1)
		color = constant_vector(m, 3, (const float[]){ 0.0f, 1.0f, 0.0f });
	store(m, texel, vec4_of(m, color, extract(m, cube, 3)));
	selection_end(m, attributes_merge);
	selection_end(m, hit_merge);
	value = load(m, m->t_v4, texel);
	if (rt_reflections_view() == 1)
	{
		/* (red where the cube map was kept) */
		unsigned int kept = op2(m, OP_F_ORD_EQUAL, m->t_bool, extract(m, value, 1), extract(m, cube, 1));

		value = select_n(m, 4, op2(m, OP_LOGICAL_AND, m->t_bool, kept, op2(m, OP_F_ORD_EQUAL, m->t_bool,
			extract(m, value, 0), extract(m, cube, 0))), constant_v4(m, 1.0f, 0.0f, 0.0f, 1.0f), value);
	}
	/* (a model's material: rough, the cube map's blur kept) */
	if (state->roughness)
		value = ext3(m, GLSL_F_MIX, m->t_v4, value, cube, splat(m, 4, ext3(m, GLSL_SMOOTH_STEP, m->t_float,
			constant_float(m, RT_REFLECTION_ROUGH), constant_float(m, RT_REFLECTION_ROUGHEST), state->roughness)));
	return value;
}

/* the occlusion at a point from a pixel's inputs (words): its static term
(a model's field relative to its reference; the lightmap pass's page's
texel, from the pre-pass) times the shapes of its cell (the dynamic
objects', or every one), then the floor and the strength: one call a pixel
(xgpu_msl.c's world_ao_evaluate; world_ao_shading.c's
world_ao_pass_occlusion) */
/* the share of a point's cosine-weighted hemisphere the dynamic objects
block, by a few rays through the distance-field scene's objects
(xgpu_msl.c's world_ao_traced, operation for operation) */
/* the rays' frame about a unit normal n (Duff's: x, y) and their turn and
twist at a pixel and frame (interleaved gradient noise) */
static void world_ao_ray_frame(struct module *m, unsigned int n, unsigned int pixel, unsigned int frame,
	unsigned int *x_out, unsigned int *y_out, unsigned int *turn_out, unsigned int *twist_out)
{
	unsigned int nx = extract(m, n, 0), ny = extract(m, n, 1), nz = extract(m, n, 2), s, a, b, px, py;

	s = fselect(m, compare(m, OP_F_ORD_GREATER_THAN_EQUAL, nz, constant_float(m, 0.0f)), constant_float(m, 1.0f),
		constant_float(m, -1.0f));
	a = fdiv(m, 1, constant_float(m, -1.0f), fadd(m, 1, s, nz));
	b = fmul(m, 1, fmul(m, 1, nx, ny), a);
	*x_out = construct(m, 3, (unsigned int[]){ fadd(m, 1, constant_float(m, 1.0f), fmul(m, 1, fmul(m, 1, fmul(m, 1, s, nx),
		nx), a)), fmul(m, 1, s, b), fneg(m, 1, fmul(m, 1, s, nx)) });
	*y_out = construct(m, 3, (unsigned int[]){ b, fadd(m, 1, s, fmul(m, 1, fmul(m, 1, ny, ny), a)), fneg(m, 1, ny) });
	px = op1(m, OP_CONVERT_S_TO_F, m->t_float, op2(m, OP_COMPOSITE_EXTRACT, m->t_int, pixel, 0));
	py = op1(m, OP_CONVERT_S_TO_F, m->t_float, op2(m, OP_COMPOSITE_EXTRACT, m->t_int, pixel, 1));
	*turn_out = ext1(m, GLSL_FRACT, m->t_float, fadd(m, 1, fmul(m, 1, constant_float(m, 52.9829189f), ext1(m, GLSL_FRACT,
		m->t_float, fadd(m, 1, fmul(m, 1, constant_float(m, 0.06711056f), px), fmul(m, 1, constant_float(m, 0.00583715f),
		py)))), fmul(m, 1, constant_float(m, 0.618034f), frame)));
	*twist_out = ext1(m, GLSL_FRACT, m->t_float, fadd(m, 1, fmul(m, 1, constant_float(m, 52.9829189f), ext1(m, GLSL_FRACT,
		m->t_float, fadd(m, 1, fmul(m, 1, constant_float(m, 0.00583715f), px), fmul(m, 1, constant_float(m, 0.06711056f),
		py)))), fmul(m, 1, constant_float(m, 0.7548777f), frame)));
}

/* a ray's direction (of XGPU_WORLD_AO_RAYS), cosine-weighted about n:
stratified in its radius, its angle turned by the golden ratio */
static unsigned int world_ao_ray_direction(struct module *m, int ray, unsigned int x, unsigned int y, unsigned int n,
	unsigned int turn, unsigned int twist)
{
	unsigned int u = fdiv(m, 1, fadd(m, 1, constant_float(m, (float)ray), turn), constant_float(m, (float)XGPU_WORLD_AO_RAYS));
	unsigned int angle = fmul(m, 1, constant_float(m, 6.28318531f), ext1(m, GLSL_FRACT, m->t_float, fadd(m, 1, twist,
		fmul(m, 1, constant_float(m, 0.618034f), constant_float(m, (float)ray)))));
	unsigned int radius = fsqrt(m, u);

	return fadd(m, 3, fadd(m, 3, fscale(m, 3, x, fmul(m, 1, radius, ext1(m, GLSL_COS, m->t_float, angle))),
		fscale(m, 3, y, fmul(m, 1, radius, ext1(m, GLSL_SIN, m->t_float, angle)))),
		fscale(m, 3, n, fsqrt(m, fmax_scalar(m, 1, fsub(m, 1, constant_float(m, 1.0f), u), 0.0f))));
}

static unsigned int world_ao_traced(struct module *m, struct pixel_state *state, struct world_ao_pass_state *pass,
	unsigned int p, unsigned int n, unsigned int pixel)
{
	unsigned int cascades[XGPU_WORLD_CASCADES], frame = extract(m, world_ao_pass_vector(m, pass, WORLD_AO_PASS_HISTORY, 0), 1);
	unsigned int x, y, turn, twist, occlusion = constant_float(m, 0.0f), origin, probe, near, voxel, near_merge, rays_merge;
	unsigned int occlusion_variable = local(m, &state->locals, m->t_float);
	int cascade, ray;

	for (cascade = 0; cascade < XGPU_WORLD_CASCADES; cascade++)
		cascades[cascade] = world_ao_pass_vector(m, pass, WORLD_AO_PASS_CASCADES, cascade);
	world_ao_ray_frame(m, n, pixel, frame, &x, &y, &turn, &twist);
	origin = fadd(m, 3, p, fscale(m, 3, n, constant_float(m, XGPU_WORLD_AO_RAY_BIAS)));
	/* (none where no object is within reach: the objects' distance there,
	in a cascade whose range holds the reach) */
	store(m, occlusion_variable, constant_float(m, 0.0f));
	probe = world_cascade_find(m, cascades, p, XGPU_WORLD_AO_RAY_CASCADE);
	selection_begin(m, op2(m, OP_LOGICAL_AND, m->t_bool, compare(m, OP_F_UNORD_NOT_EQUAL, extract(m,
		world_ao_pass_vector(m, pass, WORLD_AO_PASS_HISTORY, 0), 3), constant_float(m, 0.0f)),
		op1(m, OP_LOGICAL_NOT, m->t_bool, op2(m, OP_S_LESS_THAN, m->t_bool, probe, constant_int(m, 0)))), &near_merge);
	near = compare(m, OP_F_ORD_LESS_THAN, extract(m, world_cascade_sample(m, pass->cascades, cascades, probe, p, &voxel), 1),
		constant_float(m, XGPU_WORLD_AO_RAY_REACH));
	selection_begin(m, near, &rays_merge);
	for (ray = 0; ray < XGPU_WORLD_AO_RAYS; ray++)
	{
		unsigned int d = world_ao_ray_direction(m, ray, x, y, n, turn, twist), hit[3], f;

		world_trace(m, &state->locals, pass->cascades, cascades, NULL, origin, d, constant_float(m, XGPU_WORLD_AO_RAY_REACH),
			constant_int(m, XGPU_WORLD_TRACE_OBJECTS), hit);
		f = fdiv(m, 1, hit[1], constant_float(m, XGPU_WORLD_AO_RAY_REACH));
		occlusion = fadd(m, 1, occlusion, fmul(m, 1, hit[0], fsub(m, 1, constant_float(m, 1.0f), fmul(m, 1, f, f))));
	}
	store(m, occlusion_variable, fdiv(m, 1, occlusion, constant_float(m, (float)XGPU_WORLD_AO_RAYS)));
	selection_end(m, rays_merge);
	selection_end(m, near_merge);
	return load(m, m->t_float, occlusion_variable);
}

/* the same share by the GPU's ray queries ("ray_traced": the pass's
history[3] 2), the same rays from the same point through the ray-traced
scene (xgpu_stream.h's WORLD_AO_MESH, _INSTANCES): each the nearest front
face it meets within the reach, of an object's collision mesh (its custom
index, its owner, not the pixel's own: owner, its inputs' last word) or of
the structure's, which stops it and occludes nothing. Every pixel casts
them: the scene holds every dynamic object drawn, not the cascades' reach */
static unsigned int world_ao_traced_rays(struct module *m, struct pixel_state *state, struct world_ao_pass_state *pass,
	unsigned int p, unsigned int n, unsigned int pixel, unsigned int owner)
{
	unsigned int frame = extract(m, world_ao_pass_vector(m, pass, WORLD_AO_PASS_HISTORY, 0), 1);
	unsigned int x, y, turn, twist, origin, query_type = type0(m, OP_TYPE_RAY_QUERY);
	unsigned int query = local(m, &state->locals, query_type);
	unsigned int occlusion_variable = local(m, &state->locals, m->t_float);
	int ray;

	world_ao_ray_frame(m, n, pixel, frame, &x, &y, &turn, &twist);
	origin = fadd(m, 3, p, fscale(m, 3, n, constant_float(m, XGPU_WORLD_AO_RAY_BIAS)));
	store(m, occlusion_variable, constant_float(m, 0.0f));
	for (ray = 0; ray < XGPU_WORLD_AO_RAYS; ray++)
	{
		unsigned int d = world_ao_ray_direction(m, ray, x, y, n, turn, twist), operands[8];
		unsigned int header = id_new(m), condition = id_new(m), body = id_new(m), next = id_new(m), merge = id_new(m);
		unsigned int mine_merge, hit_merge, kind, t, f, index;

		/* rayQueryInitializeEXT(query, scene, gl_RayFlagsCullBackFacingTrianglesEXT, 0xFF, origin, 0.0, d,
		reach) */
		operands[0] = query;
		operands[1] = load(m, type0(m, OP_TYPE_ACCELERATION_STRUCTURE), pass->scene);
		operands[2] = constant_uint(m, RAY_FLAGS_CULL_BACK_FACING_TRIANGLES);
		operands[3] = constant_uint(m, XGPU_SPIRV_RT_MASK_AO);
		operands[4] = origin;
		operands[5] = constant_float(m, 0.0f);
		operands[6] = d;
		operands[7] = constant_float(m, XGPU_WORLD_AO_RAY_REACH);
		op_void(m, OP_RAY_QUERY_INITIALIZE, 8, operands);
		/* while (rayQueryProceedEXT(query)): each candidate (an object's: the
		structure's is opaque) committed but the pixel's own */
		branch(m, header);
		label_at(m, header);
		operands[0] = merge;
		operands[1] = next;
		operands[2] = 0;
		op_void(m, OP_LOOP_MERGE, 3, operands);
		branch(m, condition);
		label_at(m, condition);
		branch_conditional(m, op1(m, OP_RAY_QUERY_PROCEED, m->t_bool, query), body, merge);
		label_at(m, body);
		index = op2(m, OP_RAY_QUERY_GET_INTERSECTION_INSTANCE_CUSTOM_INDEX, m->t_int, query,
			constant_int(m, RAY_QUERY_CANDIDATE));
		selection_begin(m, rt_candidate_commits(m, &state->locals, query, op2(m, OP_I_EQUAL, m->t_bool, index,
			constant_int(m, 0)), op2(m, OP_I_NOT_EQUAL, m->t_bool, index, owner)), &mine_merge);
		op_void(m, OP_RAY_QUERY_CONFIRM_INTERSECTION, 1, &query);
		selection_end(m, mine_merge);
		branch(m, next);
		label_at(m, next);
		branch(m, header);
		label_at(m, merge);
		/* the nearest met: an object's (its custom index not 0) at t counts
		1 - (t / reach)^2 */
		kind = op2(m, OP_RAY_QUERY_GET_INTERSECTION_TYPE, m->t_uint, query, constant_int(m, RAY_QUERY_COMMITTED));
		selection_begin(m, op2(m, OP_I_NOT_EQUAL, m->t_bool, kind, constant_uint(m, 0)), &hit_merge);
		{
			unsigned int object_merge;

			index = op2(m, OP_RAY_QUERY_GET_INTERSECTION_INSTANCE_CUSTOM_INDEX, m->t_int, query,
				constant_int(m, RAY_QUERY_COMMITTED));
			selection_begin(m, op2(m, OP_I_NOT_EQUAL, m->t_bool, index, constant_int(m, 0)), &object_merge);
			t = op2(m, OP_RAY_QUERY_GET_INTERSECTION_T, m->t_float, query, constant_int(m, RAY_QUERY_COMMITTED));
			f = fdiv(m, 1, t, constant_float(m, XGPU_WORLD_AO_RAY_REACH));
			store(m, occlusion_variable, fadd(m, 1, load(m, m->t_float, occlusion_variable),
				fsub(m, 1, constant_float(m, 1.0f), fmul(m, 1, f, f))));
			selection_end(m, object_merge);
		}
		selection_end(m, hit_merge);
	}
	return fdiv(m, 1, load(m, m->t_float, occlusion_variable), constant_float(m, (float)XGPU_WORLD_AO_RAYS));
}

static unsigned int world_ao_evaluate(struct module *m, struct pixel_state *state, struct world_ao_pass_state *pass,
	unsigned int p, unsigned int words, unsigned int pixel)
{
	unsigned int word1 = uextract(m, words, 1);
	unsigned int parameters0 = world_ao_vector(m, 1, constant_int(m, 0));
	unsigned int parameters1 = world_ao_vector(m, 1, constant_int(m, 1));
	unsigned int n = world_ao_unfold(m, ext1(m, GLSL_UNPACK_SNORM_2X16, m->t_v2, uextract(m, words, 0)));
	unsigned int owner = fsub(m, 1, op1(m, OP_CONVERT_S_TO_F, m->t_float, op1(m, OP_BITCAST, m->t_int,
		uand(m, word1, constant_uint(m, 0xFFu)))), constant_float(m, 2.0f));
	unsigned int value = extract(m, ext1(m, GLSL_UNPACK_HALF_2X16, m->t_v2, op2(m, OP_SHIFT_RIGHT_LOGICAL, m->t_uint, word1,
		constant_uint(m, 16))), 0);
	unsigned int limit = op3(m, OP_SELECT, m->t_int, unot_zero(m, uand(m, word1,
		constant_uint(m, XGPU_WORLD_AO_INPUT_ALL_SHAPES))), constant_int(m, XGPU_WORLD_AO_SHAPES),
		op1(m, OP_CONVERT_F_TO_S, m->t_int, extract(m, parameters1, 1)));
	/* (the dynamic objects' shapes by the rays, their occlusion fields and
	the static scenery's shapes walked) */
	unsigned int traced = compare(m, OP_F_UNORD_NOT_EQUAL, extract(m, world_ao_pass_vector(m, pass, WORLD_AO_PASS_HISTORY,
		0), 3), constant_float(m, 0.0f));
	/* (with ray queries, the dynamic objects' occlusion fields too: their
	collision meshes are in the ray-traced scene) */
	unsigned int dynamic = op3(m, OP_SELECT, m->t_int, traced, op1(m, OP_CONVERT_F_TO_S, m->t_int, extract(m, parameters1, 1)),
		constant_int(m, 0));
	unsigned int field0 = world_ao_vector(m, 0, constant_int(m, 0)), field1 = world_ao_vector(m, 0, constant_int(m, 1));
	/* (a negative reference, a static scenery placement's own draw: its
	taps from the pixel's own depth in the field, relative to none) */
	unsigned int baked = compare(m, OP_F_ORD_LESS_THAN, value, constant_float(m, 0.0f));
	unsigned int inside = ext2(m, GLSL_F_MIN, m->t_float, world_sdf_distance(m, &state->locals, field0, field1, p),
		constant_float(m, 0.0f));
	/* (a two-sided shader's cards: about their most open direction, the
	shapes and rays too) */
	unsigned int field = world_sdf_cards_occlusion(m, state, p, &n, fselect(m, baked, inside, constant_float(m, 0.0f)),
		unot_zero(m, uand(m, word1, constant_uint(m, XGPU_WORLD_AO_INPUT_CARDS))));
	unsigned int proxies = fmul(m, 1, world_proxies_occlusion(m, state, p, n, owner, limit, dynamic,
		pass->scene ? dynamic : 0), fsub(m, 1, constant_float(m, 1.0f), pass->scene ?
		world_ao_traced_rays(m, state, pass, p, n, pixel, op1(m, OP_BITCAST, m->t_int, uextract(m, words, 3))) :
		world_ao_traced(m, state, pass, p, n, pixel)));
	unsigned int lightmap = unot_zero(m, uand(m, word1, constant_uint(m, XGPU_WORLD_AO_INPUT_LIGHTMAP)));
	unsigned int level = ext3(m, GLSL_F_MIX, m->t_float, constant_float(m, 1.0f), fmax_scalar(m, 1,
		fmul(m, 1, proxies, value), XGPU_WORLD_AO_FLOOR), extract(m, parameters0, 1));
	unsigned int relative = saturate(m, 1, fdiv(m, 1, field, fmax_scalar(m, 1, fselect(m, baked, constant_float(m, 1.0f),
		value), XGPU_WORLD_AO_REFERENCE_LEAST)));
	unsigned int model;

	field = ext3(m, GLSL_F_MIX, m->t_float, field, relative, extract(m, parameters1, 0));
	model = ext3(m, GLSL_F_MIX, m->t_float, constant_float(m, 1.0f), fmax_scalar(m, 1, fmul(m, 1,
		ext3(m, GLSL_F_MIX, m->t_float, constant_float(m, 1.0f), field, extract(m, parameters0, 2)), proxies),
		XGPU_WORLD_AO_FLOOR), extract(m, parameters0, 0));
	return fselect(m, lightmap, level, model);
}

/* the occlusion's pass: at its block's nearest pixel drawn (a quarter of
the pixels: pass.camera[3].w 2; else each pixel's own), the static term
(a model's field relative to its reference; the lightmap pass's page's
texel, from the pre-pass) times the shapes of its cell, then
the floor and the strength (world_ao_pass_occlusion); with the pixel's
distance and normal, for the surfaces' upsampling */
static void world_ao_occlusion_main(struct module *m, struct world_ao_pass_state *pass, struct pixel_state *state,
	unsigned int out)
{
	unsigned int low = op1(m, OP_CONVERT_F_TO_S, m->t_iv2, xy(m, load(m, m->t_v4, pass->frag_coord)));
	unsigned int corners = world_ao_pass_vector(m, pass, 1, 0);
	unsigned int scale = ext2(m, GLSL_F_MAX, m->t_float, extract(m, world_ao_pass_vector(m, pass, 0, 3), 3),
		constant_float(m, 1.0f));
	unsigned int iscale = op1(m, OP_CONVERT_F_TO_S, m->t_int, scale);
	unsigned int pixel = op2(m, OP_I_MUL, m->t_iv2, low, op(m, OP_COMPOSITE_CONSTRUCT, m->t_iv2, 2,
		(unsigned int[]){ iscale, iscale }));
	unsigned int nearest = constant_float(m, 3.0e38f), words = op(m, OP_COMPOSITE_CONSTRUCT, m->t_uv4, 4,
		(unsigned int[]){ constant_uint(m, 0), constant_uint(m, 0), constant_uint(m, 0), constant_uint(m, 0) });
	unsigned int p, packed, merge, present, occlusion, history, history_merge;
	int i, j;

	/* (its block's nearest pixel drawn: at most 2x2) */
	{
		unsigned int chosen = pixel;

		for (j = 0; j < 2; j++)
		{
			for (i = 0; i < 2; i++)
			{
				unsigned int at = op2(m, OP_I_ADD, m->t_iv2, pixel, op(m, OP_COMPOSITE_CONSTRUCT, m->t_iv2, 2,
					(unsigned int[]){ constant_int(m, i), constant_int(m, j) }));
				unsigned int read = world_ao_pass_fetch(m, pass->inputs, at), distance, better;

				distance = op1(m, OP_BITCAST, m->t_float, uextract(m, read, 2));
				better = op2(m, OP_LOGICAL_AND, m->t_bool, op2(m, OP_LOGICAL_AND, m->t_bool,
					world_ao_pass_inside(m, at, corners),
					unot_zero(m, uand(m, uextract(m, read, 1), constant_uint(m, XGPU_WORLD_AO_INPUT_PRESENT)))),
					compare(m, OP_F_ORD_LESS_THAN, distance, nearest));
				if (i || j)
					better = op2(m, OP_LOGICAL_AND, m->t_bool, better, op2(m, OP_S_LESS_THAN, m->t_bool,
						constant_int(m, 1), iscale));
				nearest = fselect(m, better, distance, nearest);
				words = op3(m, OP_SELECT, m->t_uv4, op(m, OP_COMPOSITE_CONSTRUCT, m->t_bv4, 4,
					(unsigned int[]){ better, better, better, better }), read, words);
				chosen = op3(m, OP_SELECT, m->t_iv2, op(m, OP_COMPOSITE_CONSTRUCT, m->t_bv2, 2,
					(unsigned int[]){ better, better }), at, chosen);
			}
		}
		pixel = chosen;
	}
	present = unot_zero(m, uand(m, uextract(m, words, 1), constant_uint(m, XGPU_WORLD_AO_INPUT_PRESENT)));
	store(m, out, construct4(m, constant_float(m, 1.0f), constant_float(m, 0.0f), constant_float(m, 0.0f),
		constant_float(m, 0.0f)));
	selection_begin(m, present, &merge);
	p = world_ao_pass_position(m, pass, pixel, nearest);
	packed = ext1(m, GLSL_UNPACK_SNORM_2X16, m->t_v2, uextract(m, words, 0));
	occlusion = world_ao_evaluate(m, state, pass, p, words, pixel);
	store(m, out, construct4(m, occlusion, nearest, extract(m, packed, 0), extract(m, packed, 1)));
	/* the previous frame's occlusion at the same point, where it is the same
	surface, blended with this frame's (xgpu_msl.c's xgpu_world_ao_pass) */
	history = world_ao_pass_vector(m, pass, WORLD_AO_PASS_HISTORY, 0);
	selection_begin(m, compare(m, OP_F_UNORD_NOT_EQUAL, extract(m, history, 0), constant_float(m, 0.0f)), &history_merge);
	{
		unsigned int previous[4], previous_corners = world_ao_pass_vector(m, pass, WORLD_AO_PASS_PREVIOUS_CORNERS, 0);
		unsigned int v, f, r, u, along, screen, low, high, at, texel, valid, valid_merge, image, operands[4], old, distance;
		unsigned int same;
		int index;

		for (index = 0; index < 4; index++)
			previous[index] = world_ao_pass_vector(m, pass, WORLD_AO_PASS_PREVIOUS, index);
		v = fsub(m, 3, p, xyz(m, previous[0]));
		f = xyz(m, previous[1]);
		r = xyz(m, previous[2]);
		u = xyz(m, previous[3]);
		along = fdiv(m, 1, fdot(m, v, f), fdot(m, f, f));
		screen = fdiv(m, 2, construct(m, 2, (unsigned int[]){ fdiv(m, 1, fdot(m, v, r), fdot(m, r, r)),
			fdiv(m, 1, fdot(m, v, u), fdot(m, u, u)) }), splat(m, 2, along));
		low = op1(m, OP_CONVERT_S_TO_F, m->t_v2, op(m, OP_VECTOR_SHUFFLE, m->t_iv2, 4,
			(unsigned int[]){ previous_corners, previous_corners, 0, 1 }));
		high = op1(m, OP_CONVERT_S_TO_F, m->t_v2, op(m, OP_VECTOR_SHUFFLE, m->t_iv2, 4,
			(unsigned int[]){ previous_corners, previous_corners, 2, 3 }));
		at = fadd(m, 2, low, fmul(m, 2, fscale(m, 2, fadd(m, 2, screen, constant_splat(m, 2, 1.0f)),
			constant_float(m, 0.5f)), fsub(m, 2, high, low)));
		texel = op1(m, OP_CONVERT_F_TO_S, m->t_iv2, ext1(m, GLSL_FLOOR, m->t_v2, fdiv(m, 2, at, splat(m, 2, scale))));
		valid = op2(m, OP_LOGICAL_AND, m->t_bool, compare(m, OP_F_ORD_GREATER_THAN, along, constant_float(m, 0.0f)),
			op2(m, OP_LOGICAL_AND, m->t_bool, op2(m, OP_LOGICAL_AND, m->t_bool,
			compare(m, OP_F_ORD_GREATER_THAN_EQUAL, extract(m, at, 0), extract(m, low, 0)),
			compare(m, OP_F_ORD_GREATER_THAN_EQUAL, extract(m, at, 1), extract(m, low, 1))),
			op2(m, OP_LOGICAL_AND, m->t_bool, compare(m, OP_F_ORD_LESS_THAN, extract(m, at, 0), extract(m, high, 0)),
			compare(m, OP_F_ORD_LESS_THAN, extract(m, at, 1), extract(m, high, 1)))));
		selection_begin(m, valid, &valid_merge);
		image = load(m, type_sampled_image(m, DIM_2D, FALSE, FALSE, TRUE), pass->history);
		operands[0] = op(m, OP_IMAGE, type_sampled_image(m, DIM_2D, FALSE, FALSE, FALSE), 1, &image);
		operands[1] = texel;
		operands[2] = IMAGE_OPERANDS_LOD;
		operands[3] = constant_int(m, 0);
		old = op(m, OP_IMAGE_FETCH, m->t_v4, 4, operands);
		distance = flength(m, v);
		same = op2(m, OP_LOGICAL_AND, m->t_bool, op2(m, OP_LOGICAL_AND, m->t_bool,
			compare(m, OP_F_ORD_GREATER_THAN, extract(m, old, 1), constant_float(m, 0.0f)),
			compare(m, OP_F_ORD_LESS_THAN_EQUAL, ext1(m, GLSL_F_ABS, m->t_float, fsub(m, 1, extract(m, old, 1), distance)),
			fmul(m, 1, constant_float(m, XGPU_WORLD_AO_HISTORY_DISTANCE), distance))),
			compare(m, OP_F_ORD_GREATER_THAN_EQUAL, fdot(m, world_ao_unfold(m, op(m, OP_VECTOR_SHUFFLE, m->t_v2, 4,
			(unsigned int[]){ old, old, 2, 3 })), world_ao_unfold(m, packed)), constant_float(m, XGPU_WORLD_AO_HISTORY_NORMAL)));
		occlusion = fselect(m, same, ext3(m, GLSL_F_MIX, m->t_float, extract(m, old, 0), occlusion, extract(m, history, 2)),
			occlusion);
		store(m, out, construct4(m, occlusion, nearest, extract(m, packed, 0), extract(m, packed, 1)));
		selection_end(m, valid_merge);
	}
	selection_end(m, history_merge);
	selection_end(m, merge);
}

BOOL xgpu_spirv_world_ao_pass(int which, const struct xgpu_spirv_options *options, struct xgpu_spirv_module *module)
{
	struct module m;
	struct pixel_state state;
	struct world_ao_pass_state pass;
	unsigned int main_id, out;

	memset(&state, 0, sizeof(state));
	state.options = options;
	if (which == XGPU_SPIRV_WORLD_AO_VERTEX)
	{
		unsigned int position_out, vertex_index, id, corner;

		module_begin(&m, EXECUTION_MODEL_VERTEX);
		position_out = variable(&m, m.t_v4, STORAGE_OUTPUT, "gl_Position");
		decorate1(&m, position_out, DECORATION_BUILT_IN, BUILT_IN_POSITION);
		vertex_index = variable(&m, m.t_int, STORAGE_INPUT, "gl_VertexIndex");
		decorate1(&m, vertex_index, DECORATION_BUILT_IN, BUILT_IN_VERTEX_INDEX);
		main_id = function_begin(&m);
		id = load(&m, m.t_int, vertex_index);
		/* corner = vec2((id << 1) & 2, id & 2) */
		corner = op1(&m, OP_CONVERT_S_TO_F, m.t_v2, op(&m, OP_COMPOSITE_CONSTRUCT, m.t_iv2, 2, (unsigned int[]){
			op2(&m, OP_BITWISE_AND, m.t_int, op2(&m, OP_SHIFT_LEFT_LOGICAL, m.t_int, id, constant_int(&m, 1)),
			constant_int(&m, 2)), op2(&m, OP_BITWISE_AND, m.t_int, id, constant_int(&m, 2)) }));
		corner = fsub(&m, 2, fscale(&m, 2, corner, constant_float(&m, 2.0f)), constant_splat(&m, 2, 1.0f));
		store(&m, position_out, construct4(&m, extract(&m, corner, 0), extract(&m, corner, 1), constant_float(&m, 0.0f),
			constant_float(&m, 1.0f)));
		if (!module_end(&m, EXECUTION_MODEL_VERTEX, main_id, &state.locals, module))
		{
			free(state.locals.words);
			return FALSE;
		}
		free(state.locals.words);
		return TRUE;
	}
	module_begin(&m, EXECUTION_MODEL_FRAGMENT);
	world_ao_declare(&m);
	world_ao_pass_declare(&m, &pass, options->ray_queries);
	out = interface_variable(&m, m.t_v4, STORAGE_OUTPUT, 0, "world_ao_occlusion");
	main_id = function_begin(&m);
	world_ao_occlusion_main(&m, &pass, &state, out);
	if (!module_end(&m, EXECUTION_MODEL_FRAGMENT, main_id, &state.locals, module))
	{
		free(state.locals.words);
		return FALSE;
	}
	free(state.locals.words);
	return TRUE;
}

/* ---------- display.shadows "ray_traced" (xgpu_spirv.h)

The pass's pixel shader, over the 3D view after its lightmap pass, the
level and the models (the pipeline's stencil test leaves the first-person
weapon's), as GLSL:

float d = texelFetch(scene_depth, ivec2(gl_FragCoord.xy), 0).r;  (1 - it, reversed)
vec3 light = vec3(1.0);
if (d < 1.0) {
  vec4 h = unproject * vec4(gl_FragCoord.xy, d, 1.0); vec3 p = h.xyz / h.w;
  float far = length(camera - p);
  vec3 from = p + (camera - p) / far * (BIAS + BIAS_FAR * far);
  vec3 n = the depth's normal (each axis's nearer neighbour), facing the camera;
  float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy - 0.5, vec2(0.06711056, 0.00583715))));
  for (int i = 0; i < count; i++) {
    vec4 a = casters[2 i], b = casters[2 i + 1];
    vec3 v = vec3(b.xy, -sqrt(max(1.0 - dot(b.xy, b.xy), 0.0)));
    vec3 rel = p - a.xyz; float along = dot(rel, v); vec3 side = rel - v * along;
    if (along > -0.5 a.w && along < 4.0 a.w && dot(side, side) < (1.5 a.w)^2) {
      float fade = along >= 0.0 ? 1.0 - along / (4.0 a.w) : 1.0 + along / (0.5 a.w);
      vec3 l = -v, x, y (Duff's frame about l); float hits = 0.0;
      for (int k = 0; k < RAYS; k++) {
        float angle = 2 pi fract(noise + 0.618034 k), r = SPREAD sqrt((k + 0.5) / RAYS);
        vec3 d = normalize(l + (x cos(angle) + y sin(angle)) * r);
        rayQueryInitializeEXT(q, scene, 0, 0xFF, from, 0.0, d, along + 1.5 a.w);
        while (rayQueryProceedEXT(q))   (an object's candidate committed if it is the caster's)
          if (owner(candidate) == owner0 || owner(candidate) == owner1) rayQueryConfirmIntersectionEXT(q);
        if (committed and its custom index not 0) hits += 1.0;   (the structure, opaque, stops it)
      }
      light *= 1.0 - unpack(b.z) * fade * clamp(dot(n, l) / FACING, 0, 1) * hits / RAYS;
    }
  }
}
fragment_light = vec4(light, 1.0);   (multiplied into the pixel by the blend) */

/* the header's (the uniforms' binding) vec4s: the unprojection's rows (the
framebuffer's x, y and depth, 1, to the world's homogeneous point), then
the camera and the casters' count */
_Static_assert(XGPU_RT_SHADOWS_RAYS == 4, "the rays' strata (xgpu_spirv_rt_shadows_pass)");

enum
{
	RT_SHADOWS_UNPROJECT = 0,
	RT_SHADOWS_CAMERA = 4,
	RT_SHADOWS_VIEW = 5,
	RT_SHADOWS_HEADER = 8,
};

/* a vec4 of a block of vec4s at an index (an id: constant or not) */
static unsigned int rt_shadows_vector(struct module *m, unsigned int block, unsigned int index)
{
	unsigned int operands[3];

	operands[0] = block;
	operands[1] = constant_int(m, 0);
	operands[2] = index;
	return load(m, m->t_v4, op(m, OP_ACCESS_CHAIN, type_pointer(m, STORAGE_UNIFORM, m->t_v4), 3, operands));
}

/* a block of count vec4s at the draw set's binding */
static unsigned int rt_shadows_block(struct module *m, unsigned int count, unsigned int binding, const char *block_name)
{
	unsigned int array = block_array(m, count), block = type_struct(m, 1, &array), id;

	decorate(m, block, DECORATION_BLOCK, 0, NULL);
	member_decorate(m, block, 0, DECORATION_OFFSET, 0);
	name(m, block, block_name);
	id = variable(m, block, STORAGE_UNIFORM, NULL);
	decorate1(m, id, DECORATION_DESCRIPTOR_SET, XGPU_SPIRV_SET_DRAW);
	decorate1(m, id, DECORATION_BINDING, binding);
	return id;
}

/* ---------- the ray-traced scene's skinning (xgpu_spirv.h's xgpu_spirv_rt_skin)

As GLSL (each a storage buffer of words):

layout(local_size_x = GROUP) in;
uint job = gl_GlobalInvocationID.y, i = gl_GlobalInvocationID.x;
uint rest = frame[job * 4], count = frame[job * 4 + 1], matrices = frame[job * 4 + 2], at = frame[job * 4 + 3];
if (i < count) {
  uint v = rest + i * 5; vec3 p = uintBitsToFloat(rests[v ..+2]); uint nodes = rests[v + 3];
  float w = uintBitsToFloat(rests[v + 4]);
  uint m0 = matrices + min(nodes & 0xFF, 63) * 12, m1 = matrices + min((nodes >> 8) & 0xFF, 63) * 12;
  for each row k: out[at + i * 3 + k] = floatBitsToUint(mix(dot(row(m1, k), vec4(p, 1)), dot(row(m0, k), vec4(p, 1)), w));
}

the game's skinned vertex programs' blend of two nodes (nv2a_vsh.c's
model programs: the position by each node's matrix, weighted). No loop: one
vertex an invocation, its job's count bounding it. */

/* a storage buffer of words at the skinning set's binding */
static unsigned int rt_skin_buffer(struct module *m, unsigned int binding, BOOL written, const char *buffer_name)
{
	unsigned int array = id_new(m), block, operands[3], id;

	operands[0] = array;
	operands[1] = m->t_uint;
	instruction(m, SECTION_GLOBALS, OP_TYPE_RUNTIME_ARRAY, 2, operands);
	decorate1(m, array, DECORATION_ARRAY_STRIDE, 4);
	block = type_struct(m, 1, &array);
	decorate(m, block, DECORATION_BLOCK, 0, NULL);
	member_decorate(m, block, 0, DECORATION_OFFSET, 0);
	if (!written)
	{
		operands[0] = block;
		operands[1] = 0;
		operands[2] = DECORATION_NON_WRITABLE;
		instruction(m, SECTION_DECORATIONS, OP_MEMBER_DECORATE, 3, operands);
	}
	id = variable(m, block, STORAGE_STORAGE_BUFFER, buffer_name);
	decorate1(m, id, DECORATION_DESCRIPTOR_SET, 0);
	decorate1(m, id, DECORATION_BINDING, binding);
	return id;
}

static unsigned int rt_skin_pointer(struct module *m, unsigned int buffer, unsigned int at)
{
	unsigned int operands[3];

	operands[0] = buffer;
	operands[1] = constant_int(m, 0);
	operands[2] = at;
	return op(m, OP_ACCESS_CHAIN, type_pointer(m, STORAGE_STORAGE_BUFFER, m->t_uint), 3, operands);
}

static unsigned int rt_skin_word(struct module *m, unsigned int buffer, unsigned int at)
{
	return load(m, m->t_uint, rt_skin_pointer(m, buffer, at));
}

static unsigned int rt_skin_float(struct module *m, unsigned int buffer, unsigned int at)
{
	return op1(m, OP_BITCAST, m->t_float, rt_skin_word(m, buffer, at));
}

BOOL xgpu_spirv_rt_skin(struct xgpu_spirv_module *module)
{
	struct module m;
	struct words locals = { 0 };
	unsigned int rests, frame, out, invocation, main_id, id, job, i, rest, count, matrices, at, merge, v, p[3], nodes, w;
	unsigned int bases[2];
	int node, row, k;

	module_begin(&m, EXECUTION_MODEL_GL_COMPUTE);
	rests = rt_skin_buffer(&m, XGPU_SPIRV_RT_SKIN_RESTS, FALSE, "rt_skin_rests");
	frame = rt_skin_buffer(&m, XGPU_SPIRV_RT_SKIN_FRAME, FALSE, "rt_skin_frame");
	out = rt_skin_buffer(&m, XGPU_SPIRV_RT_SKIN_OUT, TRUE, "rt_skin_out");
	invocation = variable(&m, type_vector(&m, m.t_uint, 3), STORAGE_INPUT, "gl_GlobalInvocationID");
	decorate1(&m, invocation, DECORATION_BUILT_IN, BUILT_IN_GLOBAL_INVOCATION_ID);
	main_id = function_begin(&m);
	id = load(&m, type_vector(&m, m.t_uint, 3), invocation);
	i = op2(&m, OP_COMPOSITE_EXTRACT, m.t_uint, id, 0);
	job = op2(&m, OP_SHIFT_LEFT_LOGICAL, m.t_uint, op2(&m, OP_COMPOSITE_EXTRACT, m.t_uint, id, 1), constant_uint(&m, 2));
	rest = rt_skin_word(&m, frame, job);
	count = rt_skin_word(&m, frame, uadd(&m, job, constant_uint(&m, 1)));
	matrices = rt_skin_word(&m, frame, uadd(&m, job, constant_uint(&m, 2)));
	at = rt_skin_word(&m, frame, uadd(&m, job, constant_uint(&m, 3)));
	selection_begin(&m, op2(&m, OP_U_LESS_THAN, m.t_bool, i, count), &merge);
	v = uadd(&m, rest, op2(&m, OP_I_MUL, m.t_uint, i, constant_uint(&m, 5)));
	for (k = 0; k < 3; k++)
		p[k] = rt_skin_float(&m, rests, uadd(&m, v, constant_uint(&m, (unsigned int)k)));
	nodes = rt_skin_word(&m, rests, uadd(&m, v, constant_uint(&m, 3)));
	w = rt_skin_float(&m, rests, uadd(&m, v, constant_uint(&m, 4)));
	for (node = 0; node < 2; node++)
	{
		unsigned int index = uand(&m, node ? op2(&m, OP_SHIFT_RIGHT_LOGICAL, m.t_uint, nodes, constant_uint(&m, 8)) : nodes,
			constant_uint(&m, 0xFFu));

		index = ext2(&m, GLSL_U_MIN, m.t_uint, index, constant_uint(&m, XGPU_SPIRV_RT_SKIN_NODES - 1));
		bases[node] = uadd(&m, matrices, op2(&m, OP_I_MUL, m.t_uint, index, constant_uint(&m, 12)));
	}
	for (row = 0; row < 3; row++)
	{
		unsigned int q[2], operands[2], value;

		for (node = 0; node < 2; node++)
		{
			unsigned int first = uadd(&m, bases[node], constant_uint(&m, (unsigned int)row * 4));

			q[node] = rt_skin_float(&m, frame, uadd(&m, first, constant_uint(&m, 3)));
			for (k = 0; k < 3; k++)
				q[node] = fadd(&m, 1, q[node], fmul(&m, 1, rt_skin_float(&m, frame, uadd(&m, first,
					constant_uint(&m, (unsigned int)k))), p[k]));
		}
		/* (the second node's, toward the first's by its weight) */
		value = fadd(&m, 1, q[1], fmul(&m, 1, fsub(&m, 1, q[0], q[1]), w));
		operands[0] = rt_skin_pointer(&m, out, uadd(&m, at, uadd(&m, op2(&m, OP_I_MUL, m.t_uint, i, constant_uint(&m, 3)),
			constant_uint(&m, (unsigned int)row))));
		operands[1] = op1(&m, OP_BITCAST, m.t_uint, value);
		op_void(&m, OP_STORE, 2, operands);
	}
	selection_end(&m, merge);
	return module_end(&m, EXECUTION_MODEL_GL_COMPUTE, main_id, &locals, module);
}

/* the scene's point at a pixel offset from this one (clamped to the image):
its world position and its depth, as the pass finds its own (a vec4) */
static unsigned int rt_shadows_point(struct module *m, unsigned int header, unsigned int raw, unsigned int pixel,
	unsigned int size, BOOL reversed, int dx, int dy)
{
	unsigned int at, operands[4], depth, framebuffer, rows[4], h;
	int row;

	/* (int vectors: construct's are float ones, which spirv-val refuses for
	int components) */
	at = ext3(m, GLSL_S_CLAMP, m->t_iv2, op2(m, OP_I_ADD, m->t_iv2, pixel, op(m, OP_COMPOSITE_CONSTRUCT, m->t_iv2, 2,
		(unsigned int[]){ constant_int(m, dx), constant_int(m, dy) })), op(m, OP_COMPOSITE_CONSTRUCT, m->t_iv2, 2,
		(unsigned int[]){ constant_int(m, 0), constant_int(m, 0) }), op2(m, OP_I_SUB, m->t_iv2, size,
		op(m, OP_COMPOSITE_CONSTRUCT, m->t_iv2, 2, (unsigned int[]){ constant_int(m, 1), constant_int(m, 1) })));
	operands[0] = raw;
	operands[1] = at;
	operands[2] = IMAGE_OPERANDS_LOD;
	operands[3] = constant_int(m, 0);
	depth = extract(m, op(m, OP_IMAGE_FETCH, m->t_v4, 4, operands), 0);
	if (reversed)
		depth = fsub(m, 1, constant_float(m, 1.0f), depth);
	{
		unsigned int centre = fadd(m, 2, op1(m, OP_CONVERT_S_TO_F, m->t_v2, at), constant_splat(m, 2, 0.5f));

		framebuffer = construct4(m, extract(m, centre, 0), extract(m, centre, 1), depth, constant_float(m, 1.0f));
	}
	for (row = 0; row < 4; row++)
		rows[row] = fdot(m, rt_shadows_vector(m, header, constant_int(m, RT_SHADOWS_UNPROJECT + row)), framebuffer);
	h = construct(m, 4, rows);
	return vec4_of(m, fdiv(m, 3, xyz(m, h), splat(m, 3, extract(m, h, 3))), depth);
}

BOOL xgpu_spirv_rt_shadows_pass(const struct xgpu_spirv_options *options, struct xgpu_spirv_module *module)
{
	struct module m;
	struct words locals = { 0 };
	unsigned int header, casters, scene_depth, frag_coord, scene, out, main_id, query_type, query;
	unsigned int light_variable, index_variable, hits_variable, operands[8];
	unsigned int coord, pixel, sampled, depth, sky_merge, h, p, camera, to_camera, far, from, noise, count;
	unsigned int header_id, condition, body, next, merge, i, a, b, bxy, v, rel, along, side, radius, inside_merge;
	unsigned int fade, l, nx, ny, nz, s, fa, fb, x, y, darkness, owners, owner0, owner1, reach, ray_light, normal;
	int ray, row, channel;
	BOOL result;

	module_begin(&m, EXECUTION_MODEL_FRAGMENT);
	header = rt_shadows_block(&m, RT_SHADOWS_HEADER, XGPU_SPIRV_BINDING_UNIFORMS, "rt_shadows_header");
	casters = rt_shadows_block(&m, XGPU_VERTEX_CONSTANT_COUNT, XGPU_SPIRV_BINDING_CONSTANTS, "rt_shadows_casters");
	scene_depth = variable(&m, type_sampled_image(&m, DIM_2D, FALSE, FALSE, TRUE), STORAGE_UNIFORM_CONSTANT, "scene_depth");
	decorate1(&m, scene_depth, DECORATION_DESCRIPTOR_SET, XGPU_SPIRV_SET_TEXTURES);
	decorate1(&m, scene_depth, DECORATION_BINDING, 0);
	frag_coord = variable(&m, m.t_v4, STORAGE_INPUT, "gl_FragCoord");
	decorate1(&m, frag_coord, DECORATION_BUILT_IN, BUILT_IN_FRAG_COORD);
	scene = variable(&m, type0(&m, OP_TYPE_ACCELERATION_STRUCTURE), STORAGE_UNIFORM_CONSTANT, "world_ao_scene");
	decorate1(&m, scene, DECORATION_DESCRIPTOR_SET, XGPU_SPIRV_SET_OCCLUSION);
	decorate1(&m, scene, DECORATION_BINDING, XGPU_SPIRV_BINDING_WORLD_AO_SCENE);
	m.ray_query = TRUE;
	out = interface_variable(&m, m.t_v4, STORAGE_OUTPUT, 0, "fragment_light");

	main_id = function_begin(&m);
	query_type = type0(&m, OP_TYPE_RAY_QUERY);
	query = local(&m, &locals, query_type);
	light_variable = local(&m, &locals, m.t_v3);
	index_variable = local(&m, &locals, m.t_int);
	hits_variable = local(&m, &locals, m.t_float);
	store(&m, light_variable, constant_splat(&m, 3, 1.0f));
	/* the scene's depth at the pixel */
	coord = load(&m, m.t_v4, frag_coord);
	pixel = op1(&m, OP_CONVERT_F_TO_S, m.t_iv2, xy(&m, coord));
	sampled = load(&m, type_sampled_image(&m, DIM_2D, FALSE, FALSE, TRUE), scene_depth);
	operands[0] = op(&m, OP_IMAGE, type_sampled_image(&m, DIM_2D, FALSE, FALSE, FALSE), 1, &sampled);
	operands[1] = pixel;
	operands[2] = IMAGE_OPERANDS_LOD;
	operands[3] = constant_int(&m, 0);
	depth = extract(&m, op(&m, OP_IMAGE_FETCH, m.t_v4, 4, operands), 0);
	if (options->reversed_depth)
		depth = fsub(&m, 1, constant_float(&m, 1.0f), depth);
	/* (none where nothing was drawn: the sky's) */
	selection_begin(&m, compare(&m, OP_F_ORD_LESS_THAN, depth, constant_float(&m, 1.0f)), &sky_merge);
	{
		unsigned int framebuffer = construct4(&m, extract(&m, coord, 0), extract(&m, coord, 1), depth,
			constant_float(&m, 1.0f)), rows[4];

		for (row = 0; row < 4; row++)
			rows[row] = fdot(&m, rt_shadows_vector(&m, header, constant_int(&m, RT_SHADOWS_UNPROJECT + row)),
				framebuffer);
		h = construct(&m, 4, rows);
		p = fdiv(&m, 3, xyz(&m, h), splat(&m, 3, extract(&m, h, 3)));
	}
	camera = rt_shadows_vector(&m, header, constant_int(&m, RT_SHADOWS_CAMERA));
	/* (within the block's casters whatever the header holds: the loop
	always ends) */
	count = ext3(&m, GLSL_S_CLAMP, m.t_int, op1(&m, OP_CONVERT_F_TO_S, m.t_int, extract(&m, camera, 3)),
		constant_int(&m, 0), constant_int(&m, XGPU_RT_SHADOWS_CASTERS));
	/* the rays' start: off the surface toward the camera, which sees it */
	to_camera = fsub(&m, 3, xyz(&m, camera), p);
	far = ext2(&m, GLSL_F_MAX, m.t_float, ext1(&m, GLSL_LENGTH, m.t_float, to_camera), constant_float(&m, 1.0e-4f));
	from = fadd(&m, 3, p, fscale(&m, 3, fdiv(&m, 3, to_camera, splat(&m, 3, far)), fadd(&m, 1,
		constant_float(&m, XGPU_RT_SHADOWS_BIAS), fmul(&m, 1, constant_float(&m, XGPU_RT_SHADOWS_BIAS_FAR), far))));
	/* the surface's normal, from the depth: along each axis the neighbour
	nearer in depth (so a silhouette's edge takes the surface's own), turned
	to face the camera. A surface facing away from a caster's light has none
	of its direct light to lose: its ray toward the light only goes back into
	its own body (a Pelican's underside, a marine's far side), and the pass
	would take its ambient light instead */
	{
		unsigned int raw = op(&m, OP_IMAGE, type_sampled_image(&m, DIM_2D, FALSE, FALSE, FALSE), 1, &sampled);
		unsigned int size = image_size(&m, raw, m.t_iv2);
		unsigned int right = rt_shadows_point(&m, header, raw, pixel, size, options->reversed_depth, 1, 0);
		unsigned int left = rt_shadows_point(&m, header, raw, pixel, size, options->reversed_depth, -1, 0);
		unsigned int down = rt_shadows_point(&m, header, raw, pixel, size, options->reversed_depth, 0, 1);
		unsigned int up = rt_shadows_point(&m, header, raw, pixel, size, options->reversed_depth, 0, -1);
		unsigned int du, dv, n, flip;

		du = select_n(&m, 3, compare(&m, OP_F_ORD_LESS_THAN,
			ext1(&m, GLSL_F_ABS, m.t_float, fsub(&m, 1, extract(&m, right, 3), depth)),
			ext1(&m, GLSL_F_ABS, m.t_float, fsub(&m, 1, depth, extract(&m, left, 3)))),
			fsub(&m, 3, xyz(&m, right), p), fsub(&m, 3, p, xyz(&m, left)));
		dv = select_n(&m, 3, compare(&m, OP_F_ORD_LESS_THAN,
			ext1(&m, GLSL_F_ABS, m.t_float, fsub(&m, 1, extract(&m, down, 3), depth)),
			ext1(&m, GLSL_F_ABS, m.t_float, fsub(&m, 1, depth, extract(&m, up, 3)))),
			fsub(&m, 3, xyz(&m, down), p), fsub(&m, 3, p, xyz(&m, up)));
		n = ext2(&m, GLSL_CROSS, m.t_v3, du, dv);
		/* (none where the neighbours give no plane: then it faces the camera) */
		n = select_n(&m, 3, compare(&m, OP_F_ORD_GREATER_THAN, fdot(&m, n, n), constant_float(&m, 1.0e-20f)), n,
			to_camera);
		n = ext1(&m, GLSL_NORMALIZE, m.t_v3, n);
		flip = compare(&m, OP_F_ORD_LESS_THAN, fdot(&m, n, to_camera), constant_float(&m, 0.0f));
		normal = select_n(&m, 3, flip, fneg(&m, 3, n), n);
	}
	/* (interleaved gradient noise: the rays turned at each pixel) */
	{
		unsigned int px = fsub(&m, 1, extract(&m, coord, 0), constant_float(&m, 0.5f));
		unsigned int py = fsub(&m, 1, extract(&m, coord, 1), constant_float(&m, 0.5f));

		noise = ext1(&m, GLSL_FRACT, m.t_float, fmul(&m, 1, constant_float(&m, 52.9829189f), ext1(&m, GLSL_FRACT,
			m.t_float, fadd(&m, 1, fmul(&m, 1, constant_float(&m, 0.06711056f), px), fmul(&m, 1,
			constant_float(&m, 0.00583715f), py)))));
	}
	/* for (int i = 0; i < count; i++) */
	header_id = id_new(&m);
	condition = id_new(&m);
	body = id_new(&m);
	next = id_new(&m);
	merge = id_new(&m);
	store(&m, index_variable, constant_int(&m, 0));
	branch(&m, header_id);
	label_at(&m, header_id);
	operands[0] = merge;
	operands[1] = next;
	operands[2] = 0;
	op_void(&m, OP_LOOP_MERGE, 3, operands);
	branch(&m, condition);
	label_at(&m, condition);
	i = load(&m, m.t_int, index_variable);
	branch_conditional(&m, op2(&m, OP_S_LESS_THAN, m.t_bool, i, count), body, merge);
	label_at(&m, body);
	a = rt_shadows_vector(&m, casters, op2(&m, OP_I_MUL, m.t_int, i, constant_int(&m, 2)));
	b = rt_shadows_vector(&m, casters, op2(&m, OP_I_ADD, m.t_int, op2(&m, OP_I_MUL, m.t_int, i, constant_int(&m, 2)),
		constant_int(&m, 1)));
	radius = extract(&m, a, 3);
	bxy = xy(&m, b);
	v = construct(&m, 3, (unsigned int[]){ extract(&m, b, 0), extract(&m, b, 1), fneg(&m, 1, fsqrt(&m, fmax_scalar(&m, 1,
		fsub(&m, 1, constant_float(&m, 1.0f), fdot(&m, bxy, bxy)), 0.0f))) });
	rel = fsub(&m, 3, p, xyz(&m, a));
	along = fdot(&m, rel, v);
	side = fsub(&m, 3, rel, fscale(&m, 3, v, along));
	/* inside its volume: half its radius toward the light, four away, its
	radius (and the penumbra's half again) to each side */
	selection_begin(&m, op2(&m, OP_LOGICAL_AND, m.t_bool, op2(&m, OP_LOGICAL_AND, m.t_bool,
		compare(&m, OP_F_ORD_GREATER_THAN, along, fmul(&m, 1, constant_float(&m, -0.5f), radius)),
		compare(&m, OP_F_ORD_LESS_THAN, along, fmul(&m, 1, constant_float(&m, 4.0f), radius))),
		compare(&m, OP_F_ORD_LESS_THAN, fdot(&m, side, side), fmul(&m, 1, fmul(&m, 1, constant_float(&m, 2.25f), radius),
		radius))), &inside_merge);
	fade = fselect(&m, compare(&m, OP_F_ORD_GREATER_THAN_EQUAL, along, constant_float(&m, 0.0f)),
		fsub(&m, 1, constant_float(&m, 1.0f), fdiv(&m, 1, along, fmul(&m, 1, constant_float(&m, 4.0f), radius))),
		fadd(&m, 1, constant_float(&m, 1.0f), fdiv(&m, 1, along, fmul(&m, 1, constant_float(&m, 0.5f), radius))));
	darkness = op1(&m, OP_BITCAST, m.t_uint, extract(&m, b, 2));
	owners = op1(&m, OP_BITCAST, m.t_uint, extract(&m, b, 3));
	owner0 = uand(&m, owners, constant_uint(&m, 0xFFFFu));
	owner1 = op2(&m, OP_SHIFT_RIGHT_LOGICAL, m.t_uint, owners, constant_uint(&m, 16));
	/* the light's direction and Duff's frame about it */
	l = fneg(&m, 3, v);
	nx = extract(&m, l, 0);
	ny = extract(&m, l, 1);
	nz = extract(&m, l, 2);
	s = fselect(&m, compare(&m, OP_F_ORD_GREATER_THAN_EQUAL, nz, constant_float(&m, 0.0f)), constant_float(&m, 1.0f),
		constant_float(&m, -1.0f));
	fa = fdiv(&m, 1, constant_float(&m, -1.0f), fadd(&m, 1, s, nz));
	fb = fmul(&m, 1, fmul(&m, 1, nx, ny), fa);
	x = construct(&m, 3, (unsigned int[]){ fadd(&m, 1, constant_float(&m, 1.0f), fmul(&m, 1, fmul(&m, 1, fmul(&m, 1, s, nx),
		nx), fa)), fmul(&m, 1, s, fb), fneg(&m, 1, fmul(&m, 1, s, nx)) });
	y = construct(&m, 3, (unsigned int[]){ fb, fadd(&m, 1, s, fmul(&m, 1, fmul(&m, 1, ny, ny), fa)), fneg(&m, 1, ny) });
	reach = fadd(&m, 1, along, fmul(&m, 1, constant_float(&m, 1.5f), radius));
	store(&m, hits_variable, constant_float(&m, 0.0f));
	for (ray = 0; ray < XGPU_RT_SHADOWS_RAYS; ray++)
	{
		unsigned int angle = fmul(&m, 1, constant_float(&m, 6.28318531f), ext1(&m, GLSL_FRACT, m.t_float, fadd(&m, 1, noise,
			constant_float(&m, 0.618034f * (float)ray))));
		/* (stratified over the cone's disc: sqrt((ray + 0.5) / RAYS)) */
		static const float stratum[4] = { 0.35355339f, 0.61237244f, 0.79056942f, 0.93541435f };
		float spread = XGPU_RT_SHADOWS_SPREAD * stratum[ray];
		unsigned int d = ext1(&m, GLSL_NORMALIZE, m.t_v3, fadd(&m, 3, l, fadd(&m, 3,
			fscale(&m, 3, x, fmul(&m, 1, constant_float(&m, spread), ext1(&m, GLSL_COS, m.t_float, angle))),
			fscale(&m, 3, y, fmul(&m, 1, constant_float(&m, spread), ext1(&m, GLSL_SIN, m.t_float, angle))))));
		unsigned int ray_header = id_new(&m), ray_condition = id_new(&m), ray_body = id_new(&m), ray_next = id_new(&m);
		unsigned int ray_merge = id_new(&m), mine_merge, hit_merge, kind, candidate, mine;

		operands[0] = query;
		operands[1] = load(&m, type0(&m, OP_TYPE_ACCELERATION_STRUCTURE), scene);
		operands[2] = constant_uint(&m, 0);
		operands[3] = constant_uint(&m, XGPU_SPIRV_RT_MASK_SHADOWS);
		operands[4] = from;
		operands[5] = constant_float(&m, 0.0f);
		operands[6] = d;
		operands[7] = reach;
		op_void(&m, OP_RAY_QUERY_INITIALIZE, 8, operands);
		branch(&m, ray_header);
		label_at(&m, ray_header);
		operands[0] = ray_merge;
		operands[1] = ray_next;
		operands[2] = 0;
		op_void(&m, OP_LOOP_MERGE, 3, operands);
		branch(&m, ray_condition);
		label_at(&m, ray_condition);
		branch_conditional(&m, op1(&m, OP_RAY_QUERY_PROCEED, m.t_bool, query), ray_body, ray_merge);
		label_at(&m, ray_body);
		candidate = uand(&m, op1(&m, OP_BITCAST, m.t_uint, op2(&m, OP_RAY_QUERY_GET_INTERSECTION_INSTANCE_CUSTOM_INDEX,
			m.t_int, query, constant_int(&m, RAY_QUERY_CANDIDATE))), constant_uint(&m, 0xFFFFu));
		mine = op2(&m, OP_LOGICAL_OR, m.t_bool, op2(&m, OP_I_EQUAL, m.t_bool, candidate, owner0),
			op2(&m, OP_LOGICAL_AND, m.t_bool, op2(&m, OP_I_NOT_EQUAL, m.t_bool, owner1, constant_uint(&m, 0)),
			op2(&m, OP_I_EQUAL, m.t_bool, candidate, owner1)));
		selection_begin(&m, rt_candidate_commits(&m, &locals, query, op2(&m, OP_I_EQUAL, m.t_bool, candidate,
			constant_uint(&m, 0)), mine), &mine_merge);
		op_void(&m, OP_RAY_QUERY_CONFIRM_INTERSECTION, 1, &query);
		selection_end(&m, mine_merge);
		branch(&m, ray_next);
		label_at(&m, ray_next);
		branch(&m, ray_header);
		label_at(&m, ray_merge);
		/* the nearest met an object's (the caster's: the structure, opaque,
		has custom index 0 and stops the ray) */
		kind = op2(&m, OP_RAY_QUERY_GET_INTERSECTION_TYPE, m.t_uint, query, constant_int(&m, RAY_QUERY_COMMITTED));
		selection_begin(&m, op2(&m, OP_I_NOT_EQUAL, m.t_bool, kind, constant_uint(&m, 0)), &hit_merge);
		{
			unsigned int object_merge, committed = op2(&m, OP_RAY_QUERY_GET_INTERSECTION_INSTANCE_CUSTOM_INDEX, m.t_int,
				query, constant_int(&m, RAY_QUERY_COMMITTED));

			selection_begin(&m, op2(&m, OP_I_NOT_EQUAL, m.t_bool, committed, constant_int(&m, 0)), &object_merge);
			store(&m, hits_variable, fadd(&m, 1, load(&m, m.t_float, hits_variable), constant_float(&m, 1.0f)));
			selection_end(&m, object_merge);
		}
		selection_end(&m, hit_merge);
	}
	/* light *= 1 - darkness * fade * hits / RAYS (darkness a byte each) */
	{
		/* (and as far as the surface faces the light: none at or past its edge,
		all from XGPU_RT_SHADOWS_FACING on) */
		unsigned int facing = saturate(&m, 1, fmul(&m, 1, fdot(&m, normal, l),
			constant_float(&m, 1.0f / XGPU_RT_SHADOWS_FACING)));
		unsigned int share = fmul(&m, 1, fmul(&m, 1, saturate(&m, 1, fade), facing), fdiv(&m, 1,
			load(&m, m.t_float, hits_variable), constant_float(&m, (float)XGPU_RT_SHADOWS_RAYS)));
		unsigned int channels[3];

		for (channel = 0; channel < 3; channel++)
			channels[channel] = fmul(&m, 1, op1(&m, OP_CONVERT_U_TO_F, m.t_float, uand(&m, op2(&m, OP_SHIFT_RIGHT_LOGICAL,
				m.t_uint, darkness, constant_uint(&m, 8u * (unsigned int)channel)), constant_uint(&m, 0xFFu))),
				constant_float(&m, 1.0f / 255.0f));
		ray_light = fsub(&m, 3, constant_splat(&m, 3, 1.0f), fscale(&m, 3, construct(&m, 3, channels), share));
		/* (HALO_VULKAN_RT_SHADOWS_VIEW, checking: a volume's pixels magenta,
		green as its rays meet the caster) */
		{
			unsigned int met = fdiv(&m, 1, load(&m, m.t_float, hits_variable),
				constant_float(&m, (float)XGPU_RT_SHADOWS_RAYS));
			unsigned int unmet = fsub(&m, 1, constant_float(&m, 1.0f), met);

			ray_light = select_n(&m, 3, compare(&m, OP_F_ORD_GREATER_THAN, extract(&m, rt_shadows_vector(&m, header,
				constant_int(&m, RT_SHADOWS_VIEW)), 0), constant_float(&m, 0.0f)), construct(&m, 3, (unsigned int[]){
				unmet, fadd(&m, 1, constant_float(&m, 0.6f), fmul(&m, 1, constant_float(&m, 0.4f), met)), unmet }),
				ray_light);
		}
		store(&m, light_variable, fmul(&m, 3, load(&m, m.t_v3, light_variable), ray_light));
	}
	selection_end(&m, inside_merge);
	branch(&m, next);
	label_at(&m, next);
	store(&m, index_variable, op2(&m, OP_I_ADD, m.t_int, i, constant_int(&m, 1)));
	branch(&m, header_id);
	label_at(&m, merge);
	selection_end(&m, sky_merge);
	store(&m, out, vec4_of(&m, load(&m, m.t_v3, light_variable), constant_float(&m, 1.0f)));
	result = module_end(&m, EXECUTION_MODEL_FRAGMENT, main_id, &locals, module);
	free(locals.words);
	return result;
}

/* ---------- display.shadows "ray_traced": the dynamic lights' shadows'
passes (xgpu_spirv.h's xgpu_spirv_light_shadows_pass; xgpu.h's
XGPU_DYNAMIC_LIGHTS_SHADOWED)

After the pre-pass, before the surfaces read it. The trace, as GLSL:

uvec4 words = texelFetch(inputs, pixel, 0);
uvec4 shadowed = cells[SHADOWED], owners = cells[SHADOWED + 2], how = cells[SHADOWED + 3];
vec4 disks = uintBitsToFloat(cells[SHADOWED + 1]), seen = vec4(1.0);
if ((words.y & PRESENT) != 0 && shadowed.x != 0) {
  vec3 p = position(pixel, uintBitsToFloat(words.z)), n = unfold(unpackSnorm2x16(words.x));
  int owner = int(words.w); float rays = clamp(float(how.x), 1.0, RAYS);
  float frame = how.y != 0 ? history.y : 0.0;   (turned each frame only where a temporal technique accumulates)
  float noise = fract(52.9829189 * fract(0.06711056 * pixel.x + 0.00583715 * pixel.y) + 0.618034 * frame);
  vec3 from = p + n * BIAS;
  for (int c = 0; c < 4; c++) if (shadowed[c] != 0) {
    vec4 light = lights.position[shadowed[c] - 1]; float w = abs(light.w);
    vec3 to = light.xyz - from; float d2 = dot(to, to);
    if (d2 * w * w < 1.0 && dot(n, to) > 0.0) {
      vec3 l = to * inversesqrt(d2), x, y (Duff's frame about l); float lit = 0.0;
      for (int k = 0; k < rays; k++) {
        float angle = 2 pi fract(noise + 0.618034 k), r = disks[c] * sqrt((k + 0.5) / rays);
        vec3 d = light.xyz + (x cos(angle) + y sin(angle)) * r - from; float far = length(d);
        rayQueryInitializeEXT(q, scene, 0, 0xFF, from, 0.0, d / far, max(far - max(SHORT disks[c], SHORT_LEAST), 0.0));
        while (rayQueryProceedEXT(q))   (an object's candidate committed but the pixel's own and the light's owner's)
          if (index(candidate) != owner && index(candidate) != owners[c]) rayQueryConfirmIntersectionEXT(q);
        lit += committed(q) == none ? 1.0 : 0.0;   (the structure, opaque, stops it)
      }
      seen[c] = lit / rays;
    }
  }
}
light_shadows = seen;

The filter (no temporal technique): each pixel's mask the mean of the 3x3
about it that are of its surface (drawn, their distances within 3% and
2 cm of its own, their normals within about 37 degrees). */

/* a float image's texel (the trace's mask, at the history's binding) */
static unsigned int light_shadows_fetch(struct module *m, unsigned int variable_id, unsigned int at)
{
	unsigned int operands[4], sampled = load(m, type_sampled_image(m, DIM_2D, FALSE, FALSE, TRUE), variable_id);

	operands[0] = op(m, OP_IMAGE, type_sampled_image(m, DIM_2D, FALSE, FALSE, FALSE), 1, &sampled);
	operands[1] = at;
	operands[2] = IMAGE_OPERANDS_LOD;
	operands[3] = constant_int(m, 0);
	return op(m, OP_IMAGE_FETCH, m->t_v4, 4, operands);
}

/* the rays toward one light's disk from a point: their share that reach
it (a float), ray queries through the scene */
static unsigned int light_shadows_rays(struct module *m, struct words *locals, struct world_ao_pass_state *pass,
	unsigned int query, unsigned int from, unsigned int center, unsigned int l, unsigned int disk, unsigned int rays,
	unsigned int noise, unsigned int owner, unsigned int light_owner, unsigned int normal)
{
	unsigned int lit_variable = local(m, locals, m->t_float);
	unsigned int nx = extract(m, l, 0), ny = extract(m, l, 1), nz = extract(m, l, 2), sign, fa, fb, x, y, shortened;
	int ray;

	sign = fselect(m, compare(m, OP_F_ORD_GREATER_THAN_EQUAL, nz, constant_float(m, 0.0f)), constant_float(m, 1.0f),
		constant_float(m, -1.0f));
	fa = fdiv(m, 1, constant_float(m, -1.0f), fadd(m, 1, sign, nz));
	fb = fmul(m, 1, fmul(m, 1, nx, ny), fa);
	x = construct(m, 3, (unsigned int[]){ fadd(m, 1, constant_float(m, 1.0f), fmul(m, 1, fmul(m, 1, fmul(m, 1, sign, nx),
		nx), fa)), fmul(m, 1, sign, fb), fneg(m, 1, fmul(m, 1, sign, nx)) });
	y = construct(m, 3, (unsigned int[]){ fb, fadd(m, 1, sign, fmul(m, 1, fmul(m, 1, ny, ny), fa)), fneg(m, 1, ny) });
	shortened = fmax_scalar(m, 1, fmul(m, 1, disk, constant_float(m, XGPU_LIGHT_SHADOWS_SHORT)),
		XGPU_LIGHT_SHADOWS_SHORT_LEAST);
	store(m, lit_variable, constant_float(m, 0.0f));
	for (ray = 0; ray < XGPU_LIGHT_SHADOWS_RAYS; ray++)
	{
		unsigned int ray_merge = 0, angle, radius, offset, d, far, operands[8];
		unsigned int header = id_new(m), condition = id_new(m), body = id_new(m), next = id_new(m), merge = id_new(m);
		unsigned int candidate, mine_merge, kind;

		if (ray)
			selection_begin(m, compare(m, OP_F_ORD_LESS_THAN, constant_float(m, (float)ray), rays), &ray_merge);
		angle = fmul(m, 1, constant_float(m, 6.28318531f), ext1(m, GLSL_FRACT, m->t_float, fadd(m, 1, noise,
			constant_float(m, 0.618034f * (float)ray))));
		radius = fmul(m, 1, disk, fsqrt(m, fdiv(m, 1, constant_float(m, (float)ray + 0.5f), rays)));
		offset = fadd(m, 3, fscale(m, 3, x, fmul(m, 1, radius, ext1(m, GLSL_COS, m->t_float, angle))),
			fscale(m, 3, y, fmul(m, 1, radius, ext1(m, GLSL_SIN, m->t_float, angle))));
		d = fsub(m, 3, fadd(m, 3, center, offset), from);
		far = fmax_scalar(m, 1, flength(m, d), 1.0e-4f);
		operands[0] = query;
		operands[1] = load(m, type0(m, OP_TYPE_ACCELERATION_STRUCTURE), pass->scene);
		operands[2] = constant_uint(m, 0);
		/* (the structure, the objects and the local player's own body, not
		the first-person weapon and hands, drawn in a view of their own; the
		body leaves its own lights' rays as their owner, below) */
		operands[3] = constant_uint(m, XGPU_SPIRV_RT_MASK_SHADOWS);
		operands[4] = from;
		operands[5] = constant_float(m, 0.0f);
		operands[6] = fdiv(m, 3, d, splat(m, 3, far));
		operands[7] = fmax_scalar(m, 1, fsub(m, 1, far, shortened), 0.0f);
		/* (HALO_LIGHT_SHADOWS_VIEW=4, checking the scene: each ray straight
		back into the surface, which must stop it) */
		if (light_shadows_view() == 4)
		{
			operands[4] = fadd(m, 3, from, fscale(m, 3, normal, constant_float(m, 0.5f)));
			operands[6] = fneg(m, 3, normal);
			operands[7] = constant_float(m, 1.0f);
		}
		/* (HALO_LIGHT_SHADOWS_VIEW=5, checking the objects: each ray toward
		HALO_LIGHT_SHADOWS_POINT, "x,y,z", inside an object, which must stop
		it) */
		if (light_shadows_view() == 5)
		{
			float point[3] = { 0.0f, 0.0f, 0.0f };
			const char *given = getenv("HALO_LIGHT_SHADOWS_POINT");
			unsigned int to_point;

			if (given)
				(void)sscanf(given, "%f,%f,%f", &point[0], &point[1], &point[2]);
			to_point = fsub(m, 3, constant_vector(m, 3, point), from);
			operands[6] = ext1(m, GLSL_NORMALIZE, m->t_v3, to_point);
			operands[7] = fmax_scalar(m, 1, fsub(m, 1, flength(m, to_point), constant_float(m, 0.05f)), 0.0f);
		}
		op_void(m, OP_RAY_QUERY_INITIALIZE, 8, operands);
		branch(m, header);
		label_at(m, header);
		operands[0] = merge;
		operands[1] = next;
		operands[2] = 0;
		op_void(m, OP_LOOP_MERGE, 3, operands);
		branch(m, condition);
		label_at(m, condition);
		branch_conditional(m, op1(m, OP_RAY_QUERY_PROCEED, m->t_bool, query), body, merge);
		label_at(m, body);
		candidate = op2(m, OP_RAY_QUERY_GET_INTERSECTION_INSTANCE_CUSTOM_INDEX, m->t_int, query,
			constant_int(m, RAY_QUERY_CANDIDATE));
		/* (HALO_LIGHT_SHADOWS_VIEW=3, checking: no owner left out) */
		selection_begin(m, rt_candidate_commits(m, locals, query, op2(m, OP_I_EQUAL, m->t_bool, candidate, constant_int(m, 0)), light_shadows_view() == 3 ? constant_bool(m, TRUE) : op2(m, OP_LOGICAL_AND, m->t_bool, op2(m, OP_I_NOT_EQUAL, m->t_bool, candidate, owner),
			op2(m, OP_I_NOT_EQUAL, m->t_bool, op1(m, OP_BITCAST, m->t_uint, candidate), light_owner))), &mine_merge);
		op_void(m, OP_RAY_QUERY_CONFIRM_INTERSECTION, 1, &query);
		selection_end(m, mine_merge);
		branch(m, next);
		label_at(m, next);
		branch(m, header);
		label_at(m, merge);
		kind = op2(m, OP_RAY_QUERY_GET_INTERSECTION_TYPE, m->t_uint, query, constant_int(m, RAY_QUERY_COMMITTED));
		store(m, lit_variable, fadd(m, 1, load(m, m->t_float, lit_variable), fselect(m,
			op2(m, OP_I_EQUAL, m->t_bool, kind, constant_uint(m, 0)), constant_float(m, 1.0f), constant_float(m, 0.0f))));
		if (ray)
			selection_end(m, ray_merge);
	}
	return fdiv(m, 1, load(m, m->t_float, lit_variable), rays);
}

static void light_shadows_trace_main(struct module *m, struct words *locals, struct world_ao_pass_state *pass,
	unsigned int out)
{
	unsigned int frag = load(m, m->t_v4, pass->frag_coord);
	unsigned int pixel = op1(m, OP_CONVERT_F_TO_S, m->t_iv2, xy(m, frag));
	unsigned int words = world_ao_pass_fetch(m, pass->inputs, pixel);
	unsigned int shadowed = light_shadows_texel(m, 0);
	unsigned int disks = op1(m, OP_BITCAST, m->t_v4, light_shadows_texel(m, 1));
	unsigned int owners = light_shadows_texel(m, 2), how = light_shadows_texel(m, 3);
	unsigned int seen_variable = local(m, locals, m->t_v4), query = local(m, locals, type0(m, OP_TYPE_RAY_QUERY));
	unsigned int present, merge, p, n, owner, rays, frame, px, py, noise, from;
	int channel;

	store(m, seen_variable, constant_splat(m, 4, 1.0f));
	present = op2(m, OP_LOGICAL_AND, m->t_bool, unot_zero(m, uand(m, uextract(m, words, 1),
		constant_uint(m, XGPU_WORLD_AO_INPUT_PRESENT))), unot_zero(m, uextract(m, shadowed, 0)));
	selection_begin(m, present, &merge);
	p = world_ao_pass_position(m, pass, pixel, op1(m, OP_BITCAST, m->t_float, uextract(m, words, 2)));
	n = world_ao_unfold(m, ext1(m, GLSL_UNPACK_SNORM_2X16, m->t_v2, uextract(m, words, 0)));
	owner = op1(m, OP_BITCAST, m->t_int, uextract(m, words, 3));
	rays = fclamp(m, 1, op1(m, OP_CONVERT_U_TO_F, m->t_float, uextract(m, how, 0)), 1.0f, (float)XGPU_LIGHT_SHADOWS_RAYS);
	frame = fselect(m, unot_zero(m, uextract(m, how, 1)), extract(m, world_ao_pass_vector(m, pass, WORLD_AO_PASS_HISTORY,
		0), 1), constant_float(m, 0.0f));
	px = op1(m, OP_CONVERT_S_TO_F, m->t_float, op2(m, OP_COMPOSITE_EXTRACT, m->t_int, pixel, 0));
	py = op1(m, OP_CONVERT_S_TO_F, m->t_float, op2(m, OP_COMPOSITE_EXTRACT, m->t_int, pixel, 1));
	noise = ext1(m, GLSL_FRACT, m->t_float, fadd(m, 1, fmul(m, 1, constant_float(m, 52.9829189f), ext1(m, GLSL_FRACT,
		m->t_float, fadd(m, 1, fmul(m, 1, constant_float(m, 0.06711056f), px), fmul(m, 1, constant_float(m, 0.00583715f),
		py)))), fmul(m, 1, constant_float(m, 0.618034f), frame)));
	from = fadd(m, 3, p, fscale(m, 3, n, constant_float(m, XGPU_LIGHT_SHADOWS_BIAS)));
	for (channel = 0; channel < XGPU_LIGHT_SHADOWS_CHANNELS; channel++)
	{
		unsigned int number = uextract(m, shadowed, (unsigned int)channel), light_merge, reach_merge;
		unsigned int light, w, to, d2, reach, share;

		selection_begin(m, unot_zero(m, number), &light_merge);
		light = dynamic_light(m, 1, op1(m, OP_BITCAST, m->t_int, op2(m, OP_I_SUB, m->t_uint, number, constant_uint(m, 1))));
		w = ext1(m, GLSL_F_ABS, m->t_float, extract(m, light, 3));
		to = fsub(m, 3, xyz(m, light), from);
		d2 = fdot(m, to, to);
		reach = op2(m, OP_LOGICAL_AND, m->t_bool, compare(m, OP_F_ORD_LESS_THAN, fmul(m, 1, fmul(m, 1, d2, w), w),
			constant_float(m, 1.0f)), compare(m, OP_F_ORD_GREATER_THAN, fdot(m, n, to), constant_float(m, 0.0f)));
		selection_begin(m, reach, &reach_merge);
		share = light_shadows_rays(m, locals, pass, query, from, xyz(m, light),
			fscale(m, 3, to, ext1(m, GLSL_INVERSE_SQRT, m->t_float, fmax_scalar(m, 1, d2, 1.0e-8f))),
			extract(m, disks, channel), rays, noise, owner, uextract(m, owners, (unsigned int)channel), n);
		/* (HALO_LIGHT_SHADOWS_VIEW=2, checking: a pixel the light reaches half
		seen, whatever its rays found) */
		if (light_shadows_view() == 2)
			share = fadd(m, 1, fmul(m, 1, share, constant_float(m, 0.0f)), constant_float(m, 0.5f));
		store(m, seen_variable, op3(m, OP_COMPOSITE_INSERT, m->t_v4, share, load(m, m->t_v4, seen_variable),
			(unsigned int)channel));
		selection_end(m, reach_merge);
		selection_end(m, light_merge);
	}
	selection_end(m, merge);
	store(m, out, load(m, m->t_v4, seen_variable));
}

static void light_shadows_filter_main(struct module *m, struct words *locals, struct world_ao_pass_state *pass,
	unsigned int out)
{
	unsigned int frag = load(m, m->t_v4, pass->frag_coord);
	unsigned int pixel = op1(m, OP_CONVERT_F_TO_S, m->t_iv2, xy(m, frag));
	unsigned int words = world_ao_pass_fetch(m, pass->inputs, pixel);
	unsigned int raw = light_shadows_fetch(m, pass->history, pixel);
	unsigned int sampled, image, last, zero, present, merge, distance, n, sum, total;
	int dx, dy;

	(void)locals;
	store(m, out, raw);
	present = unot_zero(m, uand(m, uextract(m, words, 1), constant_uint(m, XGPU_WORLD_AO_INPUT_PRESENT)));
	selection_begin(m, present, &merge);
	sampled = load(m, type_sampled_image(m, DIM_2D, FALSE, FALSE, TRUE), pass->history);
	image = op(m, OP_IMAGE, type_sampled_image(m, DIM_2D, FALSE, FALSE, FALSE), 1, &sampled);
	last = op2(m, OP_I_SUB, m->t_iv2, image_size(m, image, m->t_iv2), op(m, OP_COMPOSITE_CONSTRUCT, m->t_iv2, 2,
		(unsigned int[]){ constant_int(m, 1), constant_int(m, 1) }));
	zero = op(m, OP_COMPOSITE_CONSTRUCT, m->t_iv2, 2, (unsigned int[]){ constant_int(m, 0), constant_int(m, 0) });
	distance = op1(m, OP_BITCAST, m->t_float, uextract(m, words, 2));
	n = world_ao_unfold(m, ext1(m, GLSL_UNPACK_SNORM_2X16, m->t_v2, uextract(m, words, 0)));
	sum = raw;
	total = constant_float(m, 1.0f);
	for (dy = -1; dy <= 1; dy++)
	{
		for (dx = -1; dx <= 1; dx++)
		{
			unsigned int at, near, near_distance, same, weight;

			if (!dx && !dy)
				continue;
			at = ext3(m, GLSL_S_CLAMP, m->t_iv2, op2(m, OP_I_ADD, m->t_iv2, pixel, op(m, OP_COMPOSITE_CONSTRUCT, m->t_iv2, 2,
				(unsigned int[]){ constant_int(m, dx), constant_int(m, dy) })), zero, last);
			near = world_ao_pass_fetch(m, pass->inputs, at);
			near_distance = op1(m, OP_BITCAST, m->t_float, uextract(m, near, 2));
			same = op2(m, OP_LOGICAL_AND, m->t_bool, op2(m, OP_LOGICAL_AND, m->t_bool,
				unot_zero(m, uand(m, uextract(m, near, 1), constant_uint(m, XGPU_WORLD_AO_INPUT_PRESENT))),
				compare(m, OP_F_ORD_LESS_THAN_EQUAL, ext1(m, GLSL_F_ABS, m->t_float, fsub(m, 1, near_distance, distance)),
				fadd(m, 1, fmul(m, 1, distance, constant_float(m, 0.03f)), constant_float(m, 0.02f)))),
				compare(m, OP_F_ORD_GREATER_THAN_EQUAL, fdot(m, n, world_ao_unfold(m, ext1(m, GLSL_UNPACK_SNORM_2X16, m->t_v2,
				uextract(m, near, 0)))), constant_float(m, 0.8f)));
			weight = fselect(m, same, constant_float(m, 1.0f), constant_float(m, 0.0f));
			sum = fadd(m, 4, sum, fscale(m, 4, light_shadows_fetch(m, pass->history, at), weight));
			total = fadd(m, 1, total, weight);
		}
	}
	store(m, out, fdiv(m, 4, sum, splat(m, 4, total)));
	selection_end(m, merge);
}

/* the enhanced flashlight's shadow map (xgpu_spirv.h's
XGPU_SPIRV_LIGHT_SHADOWS_FLASHLIGHT), as GLSL:

uvec4 head = texel(FLASHLIGHT + 1); int slot = int(head.x) - 1; float R = uintBitsToFloat(head.y);
vec4 light = lights.position[texel(FLASHLIGHT)[slot] - 1]; float reach = 1.0 / abs(light.w);
vec2 ab = (gl_FragCoord.xy / SIZE * 2.0 - 1.0) * R;
vec3 d = normalize(forward + left * ab.x + up * ab.y), lamp = texel(FLASHLIGHT + 23).xyz;
rayQueryInitializeEXT(q, scene, 0, MASK_AO, lamp, 0.02, d, reach + length(lamp - light.xyz));   (no first-person models, no own body)
while (rayQueryProceedEXT(q)) if (index(candidate) != owner) commit where its alpha passes;
out = vec4(committed(q) ? t : reach, 0, 0, 1); */
static void flashlight_map_main(struct module *m, struct words *locals, struct world_ao_pass_state *pass,
	unsigned int out)
{
	unsigned int frag = load(m, m->t_v4, pass->frag_coord), head, slot, width, number, light, position, reach;
	unsigned int base, forward, up, left, a, b, d, query = local(m, locals, type0(m, OP_TYPE_RAY_QUERY));
	unsigned int owner, operands[8], header, condition, body, next, merge, candidate, mine_merge, kind, t, distance;
	unsigned int result_variable = local(m, locals, m->t_v4), slots, valid, valid_merge, lamp;
	const float size = (float)XGPU_DYNAMIC_LIGHTS_FLASHLIGHT_SHADOW_SIZE;

	store(m, result_variable, constant_vector(m, 4, (const float[]){ 1.0e4f, 0.0f, 0.0f, 1.0f }));
	head = flashlight_texel(m, constant_int(m, XGPU_DYNAMIC_LIGHTS_FLASHLIGHT + 1));
	slots = flashlight_texel(m, constant_int(m, XGPU_DYNAMIC_LIGHTS_FLASHLIGHT));
	valid = op2(m, OP_LOGICAL_AND, m->t_bool, unot_zero(m, uextract(m, head, 0)),
		op2(m, OP_U_LESS_THAN, m->t_bool, uextract(m, head, 0), constant_uint(m, XGPU_DYNAMIC_LIGHTS_FLASHLIGHTS + 1)));
	selection_begin(m, valid, &valid_merge);
	slot = int_add(m, op1(m, OP_BITCAST, m->t_int, uextract(m, head, 0)), -1);
	number = op2(m, OP_VECTOR_EXTRACT_DYNAMIC, m->t_uint, slots, slot);
	light = ext2(m, GLSL_S_MAX, m->t_int, int_add(m, op1(m, OP_BITCAST, m->t_int, number), -1), constant_int(m, 0));
	position = dynamic_light(m, 1, light);
	reach = fdiv(m, 1, constant_float(m, 1.0f), fmax_scalar(m, 1, ext1(m, GLSL_F_ABS, m->t_float, extract(m, position, 3)),
		1.0e-4f));
	width = op1(m, OP_BITCAST, m->t_float, uextract(m, head, 1));
	base = int_add(m, op2(m, OP_I_MUL, m->t_int, slot, constant_int(m, 3)), XGPU_DYNAMIC_LIGHTS_FLASHLIGHT + 2);
	forward = flashlight_vector(m, base);
	up = flashlight_vector(m, int_add(m, base, 1));
	left = flashlight_vector(m, int_add(m, base, 2));
	a = fmul(m, 1, fsub(m, 1, fmul(m, 1, extract(m, frag, 0), constant_float(m, 2.0f / size)), constant_float(m, 1.0f)),
		width);
	b = fmul(m, 1, fsub(m, 1, fmul(m, 1, extract(m, frag, 1), constant_float(m, 2.0f / size)), constant_float(m, 1.0f)),
		width);
	d = ext1(m, GLSL_NORMALIZE, m->t_v3, fadd(m, 3, forward, fadd(m, 3, fscale(m, 3, left, a), fscale(m, 3, up, b))));
	owner = op2(m, OP_COMPOSITE_EXTRACT, m->t_uint, flashlight_texel(m, constant_int(m, XGPU_DYNAMIC_LIGHTS_FLASHLIGHT + 14)),
		3);
	operands[0] = query;
	operands[1] = load(m, type0(m, OP_TYPE_ACCELERATION_STRUCTURE), pass->scene);
	operands[2] = constant_uint(m, 0);
	operands[3] = constant_uint(m, XGPU_SPIRV_RT_MASK_AO);
	/* (from the lamp, as far as the light's reach past it) */
	lamp = flashlight_vector(m, constant_int(m, XGPU_DYNAMIC_LIGHTS_FLASHLIGHT + 23));
	reach = fadd(m, 1, reach, flength(m, fsub(m, 3, lamp, xyz(m, position))));
	operands[4] = lamp;
	operands[5] = constant_float(m, 0.02f);
	operands[6] = d;
	operands[7] = reach;
	op_void(m, OP_RAY_QUERY_INITIALIZE, 8, operands);
	header = id_new(m);
	condition = id_new(m);
	body = id_new(m);
	next = id_new(m);
	merge = id_new(m);
	branch(m, header);
	label_at(m, header);
	operands[0] = merge;
	operands[1] = next;
	operands[2] = 0;
	op_void(m, OP_LOOP_MERGE, 3, operands);
	branch(m, condition);
	label_at(m, condition);
	branch_conditional(m, op1(m, OP_RAY_QUERY_PROCEED, m->t_bool, query), body, merge);
	label_at(m, body);
	candidate = op2(m, OP_RAY_QUERY_GET_INTERSECTION_INSTANCE_CUSTOM_INDEX, m->t_int, query,
		constant_int(m, RAY_QUERY_CANDIDATE));
	/* (HALO_FLASHLIGHT_SHADOW_VIEW=3, checking: no owner left out) */
	selection_begin(m, rt_candidate_commits(m, locals, query, op2(m, OP_I_EQUAL, m->t_bool, candidate, constant_int(m, 0)),
		getenv("HALO_FLASHLIGHT_SHADOW_VIEW") && atoi(getenv("HALO_FLASHLIGHT_SHADOW_VIEW")) == 3 ? constant_bool(m, TRUE) :
		op2(m, OP_I_NOT_EQUAL, m->t_bool, op1(m, OP_BITCAST, m->t_uint, candidate), owner)), &mine_merge);
	op_void(m, OP_RAY_QUERY_CONFIRM_INTERSECTION, 1, &query);
	selection_end(m, mine_merge);
	branch(m, next);
	label_at(m, next);
	branch(m, header);
	label_at(m, merge);
	kind = op2(m, OP_RAY_QUERY_GET_INTERSECTION_TYPE, m->t_uint, query, constant_int(m, RAY_QUERY_COMMITTED));
	t = op2(m, OP_RAY_QUERY_GET_INTERSECTION_T, m->t_float, query, constant_int(m, RAY_QUERY_COMMITTED));
	distance = fselect(m, op2(m, OP_I_EQUAL, m->t_bool, kind, constant_uint(m, 0)), reach, t);
	store(m, result_variable, construct4(m, distance, constant_float(m, 0.0f), constant_float(m, 0.0f),
		constant_float(m, 1.0f)));
	selection_end(m, valid_merge);
	store(m, out, load(m, m->t_v4, result_variable));
}

BOOL xgpu_spirv_light_shadows_pass(int which, const struct xgpu_spirv_options *options, struct xgpu_spirv_module *module)
{
	struct module m;
	struct words locals = { 0 };
	struct world_ao_pass_state pass;
	unsigned int main_id, out;
	BOOL result;

	if (which != XGPU_SPIRV_LIGHT_SHADOWS_FILTER && !options->ray_queries)
		return FALSE;
	module_begin(&m, EXECUTION_MODEL_FRAGMENT);
	world_ao_declare(&m);
	world_ao_pass_declare(&m, &pass, which != XGPU_SPIRV_LIGHT_SHADOWS_FILTER);
	declare_dynamic_lights(&m);
	out = interface_variable(&m, m.t_v4, STORAGE_OUTPUT, 0, "light_shadows");
	main_id = function_begin(&m);
	if (which == XGPU_SPIRV_LIGHT_SHADOWS_TRACE)
		light_shadows_trace_main(&m, &locals, &pass, out);
	else if (which == XGPU_SPIRV_LIGHT_SHADOWS_FLASHLIGHT)
		flashlight_map_main(&m, &locals, &pass, out);
	else
		light_shadows_filter_main(&m, &locals, &pass, out);
	result = module_end(&m, EXECUTION_MODEL_FRAGMENT, main_id, &locals, module);
	free(locals.words);
	return result;
}

/* ---------- display.rt_bounce: the bounced light's passes (xgpu_spirv.h's
xgpu_spirv_rt_bounce_pass; docs/rt-plan/3-gi.md, stage 3)

Only the dynamic lights' bounce: a ray's hit is lit by the block's lights
alone, never by its lightmap (whose light has its bounce baked in already).
In the enhanced lights' units a surface's light times its texture is what
it shows, so a hit shows albedo * light, and the light a surface gets back
from the hemisphere it faces is the mean of what its cosine-weighted rays
see: the receiver's texture then multiplies it, as it does the lights'.
The trace, as GLSL:

ivec2 tile = ivec2(gl_FragCoord.xy), pixel = clamp(pick(tile, frame), corners.xy, corners.zw - 1);
uvec4 words = texelFetch(inputs, pixel, 0); vec4 sample = vec4(0.0);
if ((words.y & PRESENT) != 0) {
  sample.a = 1.0;
  if (count > 0) {
    vec3 p = position(pixel, uintBitsToFloat(words.z)), n = unfold(unpackSnorm2x16(words.x));
    (Duff's x, y about n and the pixel's noise u, v, w, the AO rays')
    vec3 d = x sqrt(u) cos(2 pi v) + y sqrt(u) sin(2 pi v) + n sqrt(1 - u);
    rayQueryInitializeEXT(q, scene, CULL_BACK, MASK_AO, p + n BIAS, 0.0, d, REACH);  (the pixel's own left out)
    if (committed(q) is a triangle) {
      the hit's corners, normal (the object's to the world, toward the ray) and base map's texel (LEVEL),
      albedo = slot == 0 ? NONE : min(pow(texel.rgb, 2.2), MOST);
      for each light i: c[i] = color * window(d2 / r2)^2 * max(dot(nh, l), 0) * spot, w[i] = max(c[i]);
      W = sum(w); pick i with w[i] / W (noise w);
      if (W > 0) sample.rgb = albedo * c[i] * (W / w[i]) * (the structure's shadow ray from the hit to it reaches);
    }
  }
}

(one light by its share is the unbiased estimate of all of them with one
shadow ray: resampled importance sampling's first step.)

The resolve, a fragment a cell's face:
vec3 s = (x, y, z of the texel), w = origin + mod(s - origin, GRID);
vec4 history = valid && w inside the last grid ? texelFetch(history, texel, 0) : vec4(0.0);
where (w + 0.5) cell is in front of the eye, for the 4x4 squares about where it is seen:
  the square's sample and its pixel's position and normal; it counts where it is in w,
  max(dot(n, face), 0)^2 its weight;
h = min(history.a keep, MOST);
cache = total > 0 ? vec4((history.rgb h + sum) / (h + total), min(h + total, MOST)) : vec4(history.rgb fade, history.a);

The gather: the pixel's eight cells about p + n cell / 4, trilinear, each of
the three faces it faces by n's square along them, each by its history's
weight up to 1, normalized. */

#define RT_BOUNCE_OP_TYPE_MATRIX 24u
#define RT_BOUNCE_OP_GET_OBJECT_TO_WORLD 6031u
/* the shadow rays' mask: the structure's alone */
#define RT_BOUNCE_MASK_STRUCTURE 0x01u

struct rt_bounce_state
{
	struct world_ao_pass_state pass;
	unsigned int block;
	/* the resolve's samples, and the gather's cache (at the history's) */
	unsigned int samples;
};

static int rt_bounce_view(void)
{
	static int view = -1;

	if (view < 0)
	{
		const char *given = getenv("HALO_RT_BOUNCE_VIEW");

		view = given ? atoi(given) : 0;
	}
	return view;
}

static unsigned int rt_bounce_vector(struct module *m, struct rt_bounce_state *state, int index)
{
	return rt_shadows_vector(m, state->block, constant_int(m, index));
}

static unsigned int rt_bounce_iv2(struct module *m, unsigned int x, unsigned int y)
{
	return op(m, OP_COMPOSITE_CONSTRUCT, m->t_iv2, 2, (unsigned int[]){ x, y });
}

/* interleaved gradient noise at (x, y), turned by the frame */
static unsigned int rt_bounce_noise(struct module *m, unsigned int x, unsigned int y, unsigned int frame, float a, float b,
	float step)
{
	return ext1(m, GLSL_FRACT, m->t_float, fadd(m, 1, fmul(m, 1, constant_float(m, 52.9829189f), ext1(m, GLSL_FRACT,
		m->t_float, fadd(m, 1, fmul(m, 1, constant_float(m, a), x), fmul(m, 1, constant_float(m, b), y)))),
		fmul(m, 1, constant_float(m, step), frame)));
}

/* the pixel of a square (an ivec2) that casts its ray this frame, inside
the window's corners */
static unsigned int rt_bounce_pick(struct module *m, struct rt_bounce_state *state, unsigned int tile, unsigned int frame)
{
	unsigned int tx = op1(m, OP_CONVERT_S_TO_F, m->t_float, op2(m, OP_COMPOSITE_EXTRACT, m->t_int, tile, 0));
	unsigned int ty = op1(m, OP_CONVERT_S_TO_F, m->t_float, op2(m, OP_COMPOSITE_EXTRACT, m->t_int, tile, 1));
	unsigned int ox = ext1(m, GLSL_FLOOR, m->t_float, fmul(m, 1, rt_bounce_noise(m, tx, ty, frame, 0.06711056f, 0.00583715f,
		0.618034f), constant_float(m, (float)XGPU_RT_BOUNCE_TILE)));
	unsigned int oy = ext1(m, GLSL_FLOOR, m->t_float, fmul(m, 1, rt_bounce_noise(m, ty, tx, frame, 0.06711056f, 0.00583715f,
		0.7548777f), constant_float(m, (float)XGPU_RT_BOUNCE_TILE)));
	unsigned int at = op1(m, OP_CONVERT_F_TO_S, m->t_iv2, construct(m, 2, (unsigned int[]){
		fadd(m, 1, fmul(m, 1, tx, constant_float(m, (float)XGPU_RT_BOUNCE_TILE)), ox),
		fadd(m, 1, fmul(m, 1, ty, constant_float(m, (float)XGPU_RT_BOUNCE_TILE)), oy) }));
	unsigned int corners = world_ao_pass_vector(m, &state->pass, WORLD_AO_PASS_CORNERS, 0);
	unsigned int low = op(m, OP_VECTOR_SHUFFLE, m->t_iv2, 4, (unsigned int[]){ corners, corners, 0, 1 });
	unsigned int high = op2(m, OP_I_SUB, m->t_iv2, op(m, OP_VECTOR_SHUFFLE, m->t_iv2, 4,
		(unsigned int[]){ corners, corners, 2, 3 }), rt_bounce_iv2(m, constant_int(m, 1), constant_int(m, 1)));

	return ext3(m, GLSL_S_CLAMP, m->t_iv2, at, low, high);
}

/* a pixel's inputs: present (a bool), its position and normal */
static void rt_bounce_pixel(struct module *m, struct rt_bounce_state *state, unsigned int pixel, unsigned int *present,
	unsigned int *p, unsigned int *n, unsigned int *owner)
{
	unsigned int words = world_ao_pass_fetch(m, state->pass.inputs, pixel);

	*present = unot_zero(m, uand(m, uextract(m, words, 1), constant_uint(m, XGPU_WORLD_AO_INPUT_PRESENT)));
	*p = world_ao_pass_position(m, &state->pass, pixel, op1(m, OP_BITCAST, m->t_float, uextract(m, words, 2)));
	*n = world_ao_unfold(m, ext1(m, GLSL_UNPACK_SNORM_2X16, m->t_v2, uextract(m, words, 0)));
	if (owner)
		*owner = op1(m, OP_BITCAST, m->t_int, uextract(m, words, 3));
}

/* a ray query from origin along d to far, its candidates committed where
rt_candidate_commits takes them: objects (where objects, but the owner's)
and the structure's alpha-tested ones */
static void rt_bounce_query(struct module *m, struct words *locals, struct rt_bounce_state *state, unsigned int query,
	unsigned int flags, unsigned int mask, unsigned int origin, unsigned int d, unsigned int far, BOOL objects,
	unsigned int owner)
{
	unsigned int operands[8], candidate, mine_merge, condition_id;
	unsigned int header = id_new(m), condition = id_new(m), body = id_new(m), next = id_new(m), merge = id_new(m);

	operands[0] = query;
	operands[1] = load(m, type0(m, OP_TYPE_ACCELERATION_STRUCTURE), state->pass.scene);
	operands[2] = constant_uint(m, flags);
	operands[3] = constant_uint(m, mask);
	operands[4] = origin;
	operands[5] = constant_float(m, 0.0f);
	operands[6] = d;
	operands[7] = far;
	op_void(m, OP_RAY_QUERY_INITIALIZE, 8, operands);
	branch(m, header);
	label_at(m, header);
	operands[0] = merge;
	operands[1] = next;
	operands[2] = 0;
	op_void(m, OP_LOOP_MERGE, 3, operands);
	branch(m, condition);
	label_at(m, condition);
	branch_conditional(m, op1(m, OP_RAY_QUERY_PROCEED, m->t_bool, query), body, merge);
	label_at(m, body);
	candidate = op2(m, OP_RAY_QUERY_GET_INTERSECTION_INSTANCE_CUSTOM_INDEX, m->t_int, query,
		constant_int(m, RAY_QUERY_CANDIDATE));
	condition_id = objects ? op2(m, OP_I_NOT_EQUAL, m->t_bool, candidate, owner) : constant_bool(m, FALSE);
	selection_begin(m, rt_candidate_commits(m, locals, query, op2(m, OP_I_EQUAL, m->t_bool, candidate, constant_int(m, 0)),
		condition_id), &mine_merge);
	op_void(m, OP_RAY_QUERY_CONFIRM_INTERSECTION, 1, &query);
	selection_end(m, mine_merge);
	branch(m, next);
	label_at(m, next);
	branch(m, header);
	label_at(m, merge);
}

/* the committed hit's world normal (toward the ray: against d) and its base
map's color (linear, at most MOST) */
static void rt_bounce_hit(struct module *m, unsigned int query, unsigned int d, unsigned int *normal_out,
	unsigned int *albedo_out)
{
	unsigned int committed = constant_int(m, RAY_QUERY_COMMITTED);
	unsigned int mesh, record, primitive, corners, attributes, material_word, material, at, uv[3], normals[3], b, bx, by, st;
	unsigned int alpha_tested, slot, operands[4], pointer, sampled, texel, nobj, matrix, columns[3], nw, albedo, has_data;
	int corner;

	rt_hits_declare(m);
	mesh = uand(m, op2(m, OP_RAY_QUERY_GET_INTERSECTION_INSTANCE_SHADER_BINDING_TABLE_RECORD_OFFSET, m->t_uint, query,
		committed), constant_uint(m, XGPU_SPIRV_RT_HIT_MESHES - 1));
	record = op2(m, OP_SHIFT_LEFT_LOGICAL, m->t_uint, mesh, constant_uint(m, 2));
	/* (an alpha-tested triangle's index past the opaque ones: geometry 1) */
	alpha_tested = op2(m, OP_I_EQUAL, m->t_bool, op2(m, OP_RAY_QUERY_GET_INTERSECTION_GEOMETRY_INDEX, m->t_uint, query,
		committed), constant_uint(m, 1));
	/* (a mesh of triangles alone, a collision mesh's: its record all 0, no
	data; read as mesh 0's first words, in range, and taken as a grey wall
	facing the ray) */
	has_data = unot_zero(m, rt_hit_word(m, record));
	primitive = op3(m, OP_SELECT, m->t_uint, has_data, uadd(m, op2(m, OP_RAY_QUERY_GET_INTERSECTION_PRIMITIVE_INDEX,
		m->t_uint, query, committed), op3(m, OP_SELECT, m->t_uint, alpha_tested, rt_hit_word(m, uadd(m, record,
		constant_uint(m, 3))), constant_uint(m, 0))), constant_uint(m, 0));
	corners = uadd(m, rt_hit_word(m, record), op2(m, OP_I_MUL, m->t_uint, primitive, constant_uint(m, 3)));
	attributes = rt_hit_word(m, uadd(m, record, constant_uint(m, 1)));
	material_word = rt_hit_word(m, uadd(m, rt_hit_word(m, uadd(m, record, constant_uint(m, 2))),
		op2(m, OP_SHIFT_RIGHT_LOGICAL, m->t_uint, primitive, constant_uint(m, 1))));
	material = uand(m, op2(m, OP_SHIFT_RIGHT_LOGICAL, m->t_uint, material_word, op2(m, OP_SHIFT_LEFT_LOGICAL, m->t_uint,
		uand(m, primitive, constant_uint(m, 1)), constant_uint(m, 4))), constant_uint(m, 0xFFFFu));
	at = uadd(m, constant_uint(m, XGPU_SPIRV_RT_HIT_MATERIALS), op2(m, OP_SHIFT_LEFT_LOGICAL, m->t_uint, material,
		constant_uint(m, 4)));
	for (corner = 0; corner < 3; corner++)
	{
		unsigned int vertex = rt_hit_word(m, uadd(m, corners, constant_uint(m, (unsigned int)corner)));
		unsigned int first = uadd(m, attributes, op2(m, OP_I_MUL, m->t_uint, vertex, constant_uint(m, 6))), xy[2];

		xy[0] = rt_hit_float(m, first);
		xy[1] = rt_hit_float(m, uadd(m, first, constant_uint(m, 1)));
		uv[corner] = construct(m, 2, xy);
		normals[corner] = world_ao_unfold(m, ext1(m, GLSL_UNPACK_SNORM_2X16, m->t_v2, rt_hit_word(m, uadd(m, first,
			constant_uint(m, 2)))));
	}
	b = op2(m, OP_RAY_QUERY_GET_INTERSECTION_BARYCENTRICS, m->t_v2, query, committed);
	bx = extract(m, b, 0);
	by = extract(m, b, 1);
	st = fadd(m, 2, fadd(m, 2, uv[0], fscale(m, 2, fsub(m, 2, uv[1], uv[0]), bx)), fscale(m, 2, fsub(m, 2, uv[2], uv[0]), by));
	nobj = fadd(m, 3, fadd(m, 3, normals[0], fscale(m, 3, fsub(m, 3, normals[1], normals[0]), bx)),
		fscale(m, 3, fsub(m, 3, normals[2], normals[0]), by));
	/* (the object's space to the world's: its matrix's first three columns) */
	matrix = op2(m, RT_BOUNCE_OP_GET_OBJECT_TO_WORLD, type2(m, RT_BOUNCE_OP_TYPE_MATRIX, m->t_v3, 4), query, committed);
	for (corner = 0; corner < 3; corner++)
		columns[corner] = op2(m, OP_COMPOSITE_EXTRACT, m->t_v3, matrix, (unsigned int)corner);
	nw = fadd(m, 3, fadd(m, 3, fscale(m, 3, columns[0], extract(m, nobj, 0)), fscale(m, 3, columns[1], extract(m, nobj, 1))),
		fscale(m, 3, columns[2], extract(m, nobj, 2)));
	nw = fscale(m, 3, nw, ext1(m, GLSL_INVERSE_SQRT, m->t_float, fmax_scalar(m, 1, fdot(m, nw, nw), 1.0e-12f)));
	nw = select_n(m, 3, compare(m, OP_F_ORD_GREATER_THAN, fdot(m, nw, d), constant_float(m, 0.0f)), fneg(m, 3, nw), nw);
	/* (its base map, slot 0 none: the white stand-in, a grey taken instead) */
	slot = ext2(m, GLSL_U_MIN, m->t_uint, rt_hit_word(m, uadd(m, at, constant_uint(m, 7))),
		constant_uint(m, XGPU_SPIRV_RT_HIT_TEXTURES - 1));
	decorate(m, slot, DECORATION_NON_UNIFORM, 0, NULL);
	operands[0] = m->rt_hit_textures;
	operands[1] = slot;
	pointer = op(m, OP_ACCESS_CHAIN, type_pointer(m, STORAGE_UNIFORM_CONSTANT, type_sampled_image(m, DIM_2D, FALSE, FALSE,
		TRUE)), 2, operands);
	decorate(m, pointer, DECORATION_NON_UNIFORM, 0, NULL);
	sampled = load(m, type_sampled_image(m, DIM_2D, FALSE, FALSE, TRUE), pointer);
	decorate(m, sampled, DECORATION_NON_UNIFORM, 0, NULL);
	operands[0] = sampled;
	operands[1] = st;
	operands[2] = IMAGE_OPERANDS_LOD;
	operands[3] = constant_float(m, XGPU_RT_BOUNCE_ALBEDO_LEVEL);
	texel = op(m, OP_IMAGE_SAMPLE_EXPLICIT_LOD, m->t_v4, 4, operands);
	albedo = ext2(m, GLSL_F_MIN, m->t_v3, ext2(m, GLSL_POW, m->t_v3, saturate(m, 3, xyz(m, texel)),
		constant_splat(m, 3, 2.2f)), constant_splat(m, 3, XGPU_RT_BOUNCE_ALBEDO_MOST));
	albedo = select_n(m, 3, op2(m, OP_I_EQUAL, m->t_bool, slot, constant_uint(m, 0)),
		constant_splat(m, 3, XGPU_RT_BOUNCE_ALBEDO_NONE), albedo);
	*normal_out = select_n(m, 3, has_data, nw, fneg(m, 3, d));
	*albedo_out = select_n(m, 3, has_data, albedo, constant_splat(m, 3, XGPU_RT_BOUNCE_ALBEDO_NONE));
}

/* what a light gives a point h facing nh (a vec3: its color by its
falloff, how squarely it is faced and its cone), as the enhanced lights'
window; and its position */
static unsigned int rt_bounce_light(struct module *m, struct rt_bounce_state *state, int light, unsigned int h,
	unsigned int nh, unsigned int *position_out)
{
	unsigned int position = rt_bounce_vector(m, state, XGPU_RT_BOUNCE_BLOCK_LIGHTS + 3 * light);
	unsigned int color = rt_bounce_vector(m, state, XGPU_RT_BOUNCE_BLOCK_LIGHTS + 3 * light + 1);
	unsigned int cone = rt_bounce_vector(m, state, XGPU_RT_BOUNCE_BLOCK_LIGHTS + 3 * light + 2);
	unsigned int to = fsub(m, 3, xyz(m, position), h), d2 = fdot(m, to, to), radius = extract(m, position, 3);
	unsigned int x2 = fdiv(m, 1, d2, fmax_scalar(m, 1, fmul(m, 1, radius, radius), 1.0e-6f));
	unsigned int window = saturate(m, 1, fsub(m, 1, constant_float(m, 1.0f), fmul(m, 1, x2, x2)));
	unsigned int l = fscale(m, 3, to, ext1(m, GLSL_INVERSE_SQRT, m->t_float, fmax_scalar(m, 1, d2, 1.0e-8f)));
	unsigned int facing = fmax_scalar(m, 1, fdot(m, nh, l), 0.0f);
	unsigned int spot = saturate(m, 1, fadd(m, 1, fmul(m, 1, fdot(m, l, fneg(m, 3, xyz(m, cone))), extract(m, cone, 3)),
		extract(m, color, 3)));
	unsigned int counted = compare(m, OP_F_ORD_LESS_THAN, constant_float(m, (float)light),
		extract(m, rt_bounce_vector(m, state, XGPU_RT_BOUNCE_BLOCK_EYE), 3));

	*position_out = xyz(m, position);
	return select_n(m, 3, counted, fscale(m, 3, xyz(m, color), fmul(m, 1, fmul(m, 1, fmul(m, 1, window, window), facing),
		spot)), constant_splat(m, 3, 0.0f));
}

static void rt_bounce_trace_main(struct module *m, struct words *locals, struct rt_bounce_state *state, unsigned int out)
{
	unsigned int frag = load(m, m->t_v4, state->pass.frag_coord);
	unsigned int tile = op1(m, OP_CONVERT_F_TO_S, m->t_iv2, xy(m, frag));
	unsigned int frame = extract(m, world_ao_pass_vector(m, &state->pass, WORLD_AO_PASS_HISTORY, 0), 1);
	unsigned int pixel = rt_bounce_pick(m, state, tile, frame);
	unsigned int query = local(m, locals, type0(m, OP_TYPE_RAY_QUERY));
	unsigned int present, p, n, owner, present_merge, lights_merge, hit_merge, lit_merge;
	unsigned int x, y, u, v, radius, angle, d, origin, kind, h, nh, albedo, px, py, pick;
	unsigned int contributions[XGPU_RT_BOUNCE_BLOCK_LIGHTS_MOST], positions[XGPU_RT_BOUNCE_BLOCK_LIGHTS_MOST];
	unsigned int weights[XGPU_RT_BOUNCE_BLOCK_LIGHTS_MOST], total, target, prefix, found, chosen, chosen_at;
	int light;

	store(m, out, constant_splat(m, 4, 0.0f));
	rt_bounce_pixel(m, state, pixel, &present, &p, &n, &owner);
	selection_begin(m, present, &present_merge);
	store(m, out, constant_vector(m, 4, (const float[]){ 0.0f, 0.0f, 0.0f, 1.0f }));
	selection_begin(m, compare(m, OP_F_ORD_GREATER_THAN, extract(m, rt_bounce_vector(m, state, XGPU_RT_BOUNCE_BLOCK_EYE),
		3), constant_float(m, 0.0f)), &lights_merge);
	world_ao_ray_frame(m, n, pixel, frame, &x, &y, &u, &v);
	radius = fsqrt(m, u);
	angle = fmul(m, 1, constant_float(m, 6.28318531f), v);
	d = fadd(m, 3, fadd(m, 3, fscale(m, 3, x, fmul(m, 1, radius, ext1(m, GLSL_COS, m->t_float, angle))),
		fscale(m, 3, y, fmul(m, 1, radius, ext1(m, GLSL_SIN, m->t_float, angle)))),
		fscale(m, 3, n, fsqrt(m, fmax_scalar(m, 1, fsub(m, 1, constant_float(m, 1.0f), u), 0.0f))));
	origin = fadd(m, 3, p, fscale(m, 3, n, constant_float(m, XGPU_RT_BOUNCE_BIAS)));
	rt_bounce_query(m, locals, state, query, RAY_FLAGS_CULL_BACK_FACING_TRIANGLES, XGPU_SPIRV_RT_MASK_AO, origin, d,
		constant_float(m, XGPU_RT_BOUNCE_REACH), TRUE, owner);
	kind = op2(m, OP_RAY_QUERY_GET_INTERSECTION_TYPE, m->t_uint, query, constant_int(m, RAY_QUERY_COMMITTED));
	selection_begin(m, op2(m, OP_I_NOT_EQUAL, m->t_bool, kind, constant_uint(m, 0)), &hit_merge);
	h = fadd(m, 3, origin, fscale(m, 3, d, op2(m, OP_RAY_QUERY_GET_INTERSECTION_T, m->t_float, query,
		constant_int(m, RAY_QUERY_COMMITTED))));
	rt_bounce_hit(m, query, d, &nh, &albedo);
	/* (the lights' shares, and one chosen by them) */
	total = constant_float(m, 0.0f);
	for (light = 0; light < XGPU_RT_BOUNCE_BLOCK_LIGHTS_MOST; light++)
	{
		contributions[light] = rt_bounce_light(m, state, light, h, nh, &positions[light]);
		weights[light] = ext2(m, GLSL_F_MAX, m->t_float, extract(m, contributions[light], 0), ext2(m, GLSL_F_MAX, m->t_float,
			extract(m, contributions[light], 1), extract(m, contributions[light], 2)));
		total = fadd(m, 1, total, weights[light]);
	}
	px = op1(m, OP_CONVERT_S_TO_F, m->t_float, op2(m, OP_COMPOSITE_EXTRACT, m->t_int, pixel, 0));
	py = op1(m, OP_CONVERT_S_TO_F, m->t_float, op2(m, OP_COMPOSITE_EXTRACT, m->t_int, pixel, 1));
	pick = rt_bounce_noise(m, fadd(m, 1, px, constant_float(m, 37.0f)), fadd(m, 1, py, constant_float(m, 11.0f)), frame,
		0.06711056f, 0.00583715f, 0.5698403f);
	target = fmul(m, 1, pick, total);
	prefix = constant_float(m, 0.0f);
	found = constant_bool(m, FALSE);
	chosen = constant_splat(m, 3, 0.0f);
	chosen_at = constant_splat(m, 3, 0.0f);
	for (light = 0; light < XGPU_RT_BOUNCE_BLOCK_LIGHTS_MOST; light++)
	{
		unsigned int take;

		prefix = fadd(m, 1, prefix, weights[light]);
		take = op2(m, OP_LOGICAL_AND, m->t_bool, op1(m, OP_LOGICAL_NOT, m->t_bool, found), op2(m, OP_LOGICAL_AND, m->t_bool,
			compare(m, OP_F_ORD_LESS_THAN, target, prefix), compare(m, OP_F_ORD_GREATER_THAN, weights[light],
			constant_float(m, 0.0f))));
		chosen = select_n(m, 3, take, fscale(m, 3, contributions[light], fdiv(m, 1, total, fmax_scalar(m, 1, weights[light],
			1.0e-12f))), chosen);
		chosen_at = select_n(m, 3, take, positions[light], chosen_at);
		found = op2(m, OP_LOGICAL_OR, m->t_bool, found, take);
	}
	selection_begin(m, found, &lit_merge);
	{
		unsigned int from = fadd(m, 3, h, fscale(m, 3, nh, constant_float(m, XGPU_RT_BOUNCE_BIAS)));
		unsigned int to = fsub(m, 3, chosen_at, from), far = fmax_scalar(m, 1, flength(m, to), 1.0e-4f), seen;

		rt_bounce_query(m, locals, state, query, 0, RT_BOUNCE_MASK_STRUCTURE, from, fdiv(m, 3, to, splat(m, 3, far)),
			fmax_scalar(m, 1, fsub(m, 1, far, constant_float(m, 0.05f)), 0.0f), FALSE, 0);
		seen = op2(m, OP_I_EQUAL, m->t_bool, op2(m, OP_RAY_QUERY_GET_INTERSECTION_TYPE, m->t_uint, query,
			constant_int(m, RAY_QUERY_COMMITTED)), constant_uint(m, 0));
		store(m, out, vec4_of(m, select_n(m, 3, seen, fmul(m, 3, albedo, chosen), constant_splat(m, 3, 0.0f)),
			constant_float(m, 1.0f)));
	}
	selection_end(m, lit_merge);
	selection_end(m, hit_merge);
	selection_end(m, lights_merge);
	selection_end(m, present_merge);
}

/* a float's floor's float of a vec3's: floor(a / b) * b's remainder, a
modulo b (b positive) */
static unsigned int rt_bounce_modulo(struct module *m, unsigned int a, unsigned int b)
{
	return fsub(m, 3, a, fmul(m, 3, b, ext1(m, GLSL_FLOOR, m->t_v3, fdiv(m, 3, a, b))));
}

static unsigned int rt_bounce_all3(struct module *m, unsigned int opcode, unsigned int a, unsigned int b)
{
	return op2(m, OP_LOGICAL_AND, m->t_bool, op2(m, OP_LOGICAL_AND, m->t_bool,
		compare(m, opcode, extract(m, a, 0), extract(m, b, 0)), compare(m, opcode, extract(m, a, 1), extract(m, b, 1))),
		compare(m, opcode, extract(m, a, 2), extract(m, b, 2)));
}

static void rt_bounce_resolve_main(struct module *m, struct words *locals, struct rt_bounce_state *state, unsigned int out)
{
	unsigned int frag = load(m, m->t_v4, state->pass.frag_coord);
	unsigned int texel = op1(m, OP_CONVERT_F_TO_S, m->t_iv2, xy(m, frag));
	unsigned int fx = op1(m, OP_CONVERT_S_TO_F, m->t_float, op2(m, OP_COMPOSITE_EXTRACT, m->t_int, texel, 0));
	unsigned int fy = op1(m, OP_CONVERT_S_TO_F, m->t_float, op2(m, OP_COMPOSITE_EXTRACT, m->t_int, texel, 1));
	unsigned int origin_vector = rt_bounce_vector(m, state, XGPU_RT_BOUNCE_BLOCK_ORIGIN);
	unsigned int previous = rt_bounce_vector(m, state, XGPU_RT_BOUNCE_BLOCK_PREVIOUS);
	unsigned int times = rt_bounce_vector(m, state, XGPU_RT_BOUNCE_BLOCK_TIMES);
	unsigned int sizes = rt_bounce_vector(m, state, XGPU_RT_BOUNCE_BLOCK_SIZES);
	unsigned int eye = xyz(m, rt_bounce_vector(m, state, XGPU_RT_BOUNCE_BLOCK_EYE));
	unsigned int origin = xyz(m, origin_vector), cell = extract(m, origin_vector, 3);
	unsigned int dims = constant_vector(m, 3, (const float[]){ (float)XGPU_RT_BOUNCE_GRID_X, (float)XGPU_RT_BOUNCE_GRID_Y,
		(float)XGPU_RT_BOUNCE_GRID_Z });
	unsigned int face, sx, sz, sy, s, w, valid, history, center, offset, a, b, c, front, corners, low, size, screen, tile;
	unsigned int axis, sign, fn, sum_variable = local(m, locals, m->t_v3), total_variable = local(m, locals, m->t_float);
	unsigned int front_merge, last, zero, frame, kept, got, sum, total, value, weight_out;
	int dx, dy;

	face = ext1(m, GLSL_FLOOR, m->t_float, fdiv(m, 1, fx, constant_float(m, (float)XGPU_RT_BOUNCE_GRID_X)));
	sx = fsub(m, 1, fx, fmul(m, 1, face, constant_float(m, (float)XGPU_RT_BOUNCE_GRID_X)));
	sz = ext1(m, GLSL_FLOOR, m->t_float, fdiv(m, 1, fy, constant_float(m, (float)XGPU_RT_BOUNCE_GRID_Y)));
	sy = fsub(m, 1, fy, fmul(m, 1, sz, constant_float(m, (float)XGPU_RT_BOUNCE_GRID_Y)));
	s = construct(m, 3, (unsigned int[]){ sx, sy, sz });
	w = fadd(m, 3, origin, rt_bounce_modulo(m, fsub(m, 3, s, origin), dims));
	valid = op2(m, OP_LOGICAL_AND, m->t_bool, op2(m, OP_LOGICAL_AND, m->t_bool,
		compare(m, OP_F_ORD_GREATER_THAN, extract(m, previous, 3), constant_float(m, 0.5f)),
		rt_bounce_all3(m, OP_F_ORD_GREATER_THAN_EQUAL, w, xyz(m, previous))),
		rt_bounce_all3(m, OP_F_ORD_LESS_THAN, w, fadd(m, 3, xyz(m, previous), dims)));
	history = select_n(m, 4, valid, light_shadows_fetch(m, state->pass.history, texel), constant_splat(m, 4, 0.0f));
	/* (the face's way: +x, -x, +y, -y, +z, -z) */
	axis = ext1(m, GLSL_FLOOR, m->t_float, fmul(m, 1, face, constant_float(m, 0.5f)));
	sign = fsub(m, 1, constant_float(m, 1.0f), fmul(m, 1, constant_float(m, 2.0f), fsub(m, 1, face, fmul(m, 1, axis,
		constant_float(m, 2.0f)))));
	fn = construct(m, 3, (unsigned int[]){
		fselect(m, compare(m, OP_F_ORD_EQUAL, axis, constant_float(m, 0.0f)), sign, constant_float(m, 0.0f)),
		fselect(m, compare(m, OP_F_ORD_EQUAL, axis, constant_float(m, 1.0f)), sign, constant_float(m, 0.0f)),
		fselect(m, compare(m, OP_F_ORD_EQUAL, axis, constant_float(m, 2.0f)), sign, constant_float(m, 0.0f)) });
	/* (where its middle is seen: (s, s x, s y) of its offset from the eye) */
	center = fscale(m, 3, fadd(m, 3, w, constant_splat(m, 3, 0.5f)), cell);
	offset = fsub(m, 3, center, eye);
	a = fdot(m, xyz(m, rt_bounce_vector(m, state, XGPU_RT_BOUNCE_BLOCK_UNPROJECT)), offset);
	b = fdot(m, xyz(m, rt_bounce_vector(m, state, XGPU_RT_BOUNCE_BLOCK_UNPROJECT + 1)), offset);
	c = fdot(m, xyz(m, rt_bounce_vector(m, state, XGPU_RT_BOUNCE_BLOCK_UNPROJECT + 2)), offset);
	front = compare(m, OP_F_ORD_GREATER_THAN, a, constant_float(m, 1.0e-3f));
	store(m, sum_variable, constant_splat(m, 3, 0.0f));
	store(m, total_variable, constant_float(m, 0.0f));
	selection_begin(m, front, &front_merge);
	corners = world_ao_pass_vector(m, &state->pass, WORLD_AO_PASS_CORNERS, 0);
	low = op1(m, OP_CONVERT_S_TO_F, m->t_v2, op(m, OP_VECTOR_SHUFFLE, m->t_iv2, 4, (unsigned int[]){ corners, corners, 0, 1 }));
	size = fsub(m, 2, op1(m, OP_CONVERT_S_TO_F, m->t_v2, op(m, OP_VECTOR_SHUFFLE, m->t_iv2, 4,
		(unsigned int[]){ corners, corners, 2, 3 })), low);
	screen = construct(m, 2, (unsigned int[]){ fdiv(m, 1, b, a), fdiv(m, 1, c, a) });
	screen = fadd(m, 2, low, fmul(m, 2, fscale(m, 2, fadd(m, 2, screen, constant_splat(m, 2, 1.0f)), constant_float(m, 0.5f)),
		size));
	/* (far off the screen: none of it seen; clamped, so the float fits) */
	screen = ext3(m, GLSL_F_CLAMP, m->t_v2, screen, constant_splat(m, 2, -65536.0f), constant_splat(m, 2, 65536.0f));
	tile = op1(m, OP_CONVERT_F_TO_S, m->t_iv2, ext1(m, GLSL_FLOOR, m->t_v2, fscale(m, 2, screen,
		constant_float(m, 1.0f / (float)XGPU_RT_BOUNCE_TILE))));
	last = op1(m, OP_CONVERT_F_TO_S, m->t_iv2, fsub(m, 2, construct(m, 2, (unsigned int[]){ extract(m, sizes, 1),
		extract(m, sizes, 2) }), constant_splat(m, 2, 1.0f)));
	zero = rt_bounce_iv2(m, constant_int(m, 0), constant_int(m, 0));
	frame = extract(m, world_ao_pass_vector(m, &state->pass, WORLD_AO_PASS_HISTORY, 0), 1);
	sum = constant_splat(m, 3, 0.0f);
	total = constant_float(m, 0.0f);
	for (dy = -2; dy < 2; dy++)
	{
		for (dx = -2; dx < 2; dx++)
		{
			unsigned int at = ext3(m, GLSL_S_CLAMP, m->t_iv2, op2(m, OP_I_ADD, m->t_iv2, tile, rt_bounce_iv2(m,
				constant_int(m, dx), constant_int(m, dy))), zero, last);
			unsigned int sample = light_shadows_fetch(m, state->samples, at);
			unsigned int pixel = rt_bounce_pick(m, state, at, frame), present, p, n, inside, facing, weight;

			rt_bounce_pixel(m, state, pixel, &present, &p, &n, NULL);
			inside = rt_bounce_all3(m, OP_F_ORD_EQUAL, ext1(m, GLSL_FLOOR, m->t_v3, fscale(m, 3, p, fdiv(m, 1,
				constant_float(m, 1.0f), cell))), w);
			facing = fmax_scalar(m, 1, fdot(m, n, fn), 0.0f);
			weight = fselect(m, op2(m, OP_LOGICAL_AND, m->t_bool, op2(m, OP_LOGICAL_AND, m->t_bool, present, inside),
				compare(m, OP_F_ORD_GREATER_THAN, extract(m, sample, 3), constant_float(m, 0.5f))),
				fmul(m, 1, facing, facing), constant_float(m, 0.0f));
			sum = fadd(m, 3, sum, fscale(m, 3, xyz(m, sample), weight));
			total = fadd(m, 1, total, weight);
		}
	}
	store(m, sum_variable, sum);
	store(m, total_variable, total);
	selection_end(m, front_merge);
	sum = load(m, m->t_v3, sum_variable);
	total = load(m, m->t_float, total_variable);
	kept = ext2(m, GLSL_F_MIN, m->t_float, fmul(m, 1, extract(m, history, 3), extract(m, times, 1)), extract(m, times, 2));
	got = compare(m, OP_F_ORD_GREATER_THAN, total, constant_float(m, 0.0f));
	value = select_n(m, 3, got, fdiv(m, 3, fadd(m, 3, fscale(m, 3, xyz(m, history), kept), sum),
		splat(m, 3, fmax_scalar(m, 1, fadd(m, 1, kept, total), 1.0e-6f))), fscale(m, 3, xyz(m, history), extract(m, times, 3)));
	weight_out = fselect(m, got, ext2(m, GLSL_F_MIN, m->t_float, fadd(m, 1, kept, total), extract(m, times, 2)),
		extract(m, history, 3));
	store(m, out, vec4_of(m, value, weight_out));
}

static void rt_bounce_gather_main(struct module *m, struct words *locals, struct rt_bounce_state *state, unsigned int out)
{
	unsigned int frag = load(m, m->t_v4, state->pass.frag_coord);
	unsigned int pixel = op1(m, OP_CONVERT_F_TO_S, m->t_iv2, xy(m, frag));
	unsigned int origin_vector = rt_bounce_vector(m, state, XGPU_RT_BOUNCE_BLOCK_ORIGIN);
	unsigned int origin = xyz(m, origin_vector), cell = extract(m, origin_vector, 3);
	unsigned int dims = constant_vector(m, 3, (const float[]){ (float)XGPU_RT_BOUNCE_GRID_X, (float)XGPU_RT_BOUNCE_GRID_Y,
		(float)XGPU_RT_BOUNCE_GRID_Z });
	unsigned int present, p, n, merge, g, base, f, value, total, faces[3], face_weights[3];
	int corner, axis;

	(void)locals;
	store(m, out, constant_splat(m, 4, 0.0f));
	rt_bounce_pixel(m, state, pixel, &present, &p, &n, NULL);
	selection_begin(m, present, &merge);
	g = fsub(m, 3, fscale(m, 3, fadd(m, 3, p, fscale(m, 3, n, fmul(m, 1, cell, constant_float(m, 0.25f)))), fdiv(m, 1,
		constant_float(m, 1.0f), cell)), constant_splat(m, 3, 0.5f));
	base = ext1(m, GLSL_FLOOR, m->t_v3, g);
	f = fsub(m, 3, g, base);
	for (axis = 0; axis < 3; axis++)
	{
		unsigned int component = extract(m, n, (unsigned int)axis);

		faces[axis] = fadd(m, 1, constant_float(m, (float)(2 * axis)), fselect(m, compare(m, OP_F_ORD_LESS_THAN, component,
			constant_float(m, 0.0f)), constant_float(m, 1.0f), constant_float(m, 0.0f)));
		face_weights[axis] = fmul(m, 1, component, component);
	}
	value = constant_splat(m, 3, 0.0f);
	total = constant_float(m, 0.0f);
	for (corner = 0; corner < 8; corner++)
	{
		const float step[3] = { (float)(corner & 1), (float)((corner >> 1) & 1), (float)((corner >> 2) & 1) };
		unsigned int wc = fadd(m, 3, base, constant_vector(m, 3, step)), inside, slot, trilinear = constant_float(m, 1.0f);

		for (axis = 0; axis < 3; axis++)
		{
			unsigned int fa = extract(m, f, (unsigned int)axis);

			trilinear = fmul(m, 1, trilinear, step[axis] > 0.5f ? fa : fsub(m, 1, constant_float(m, 1.0f), fa));
		}
		inside = op2(m, OP_LOGICAL_AND, m->t_bool, rt_bounce_all3(m, OP_F_ORD_GREATER_THAN_EQUAL, wc, origin),
			rt_bounce_all3(m, OP_F_ORD_LESS_THAN, wc, fadd(m, 3, origin, dims)));
		trilinear = fselect(m, inside, trilinear, constant_float(m, 0.0f));
		slot = rt_bounce_modulo(m, wc, dims);
		for (axis = 0; axis < 3; axis++)
		{
			unsigned int at = op1(m, OP_CONVERT_F_TO_S, m->t_iv2, construct(m, 2, (unsigned int[]){
				fadd(m, 1, extract(m, slot, 0), fmul(m, 1, faces[axis], constant_float(m, (float)XGPU_RT_BOUNCE_GRID_X))),
				fadd(m, 1, extract(m, slot, 1), fmul(m, 1, extract(m, slot, 2), constant_float(m, (float)XGPU_RT_BOUNCE_GRID_Y))) }));
			unsigned int cached = light_shadows_fetch(m, state->pass.history, at);
			unsigned int weight = fmul(m, 1, fmul(m, 1, trilinear, face_weights[axis]), saturate(m, 1, extract(m, cached, 3)));

			value = fadd(m, 3, value, fscale(m, 3, xyz(m, cached), weight));
			total = fadd(m, 1, total, weight);
		}
	}
	value = fscale(m, 3, value, fdiv(m, 1, extract(m, rt_bounce_vector(m, state, XGPU_RT_BOUNCE_BLOCK_SIZES), 3),
		fmax_scalar(m, 1, total, 0.5f)));
	/* (HALO_RT_BOUNCE_VIEW, checking: 1 magenta where the surfaces read it,
	2 the trace's samples, 3 the cache's weight in green over its light) */
	if (rt_bounce_view() == 1)
		value = constant_vector(m, 3, (const float[]){ 1.0f, 0.0f, 1.0f });
	else if (rt_bounce_view() == 2)
		value = fscale(m, 3, xyz(m, light_shadows_fetch(m, state->samples, op2(m, OP_SHIFT_RIGHT_ARITHMETIC, m->t_iv2, pixel,
			rt_bounce_iv2(m, constant_int(m, 2), constant_int(m, 2))))), constant_float(m, 10.0f));
	else if (rt_bounce_view() == 5)
	{
		/* (where the pixel's own cell's middle is seen, against the pixel: red x, green y, a 200th of a pixel) */
		unsigned int center = fscale(m, 3, fadd(m, 3, ext1(m, GLSL_FLOOR, m->t_v3, fscale(m, 3, p, fdiv(m, 1,
			constant_float(m, 1.0f), cell))), constant_splat(m, 3, 0.5f)), cell);
		unsigned int offset = fsub(m, 3, center, xyz(m, rt_bounce_vector(m, state, XGPU_RT_BOUNCE_BLOCK_EYE)));
		unsigned int a = fdot(m, xyz(m, rt_bounce_vector(m, state, XGPU_RT_BOUNCE_BLOCK_UNPROJECT)), offset);
		unsigned int b = fdot(m, xyz(m, rt_bounce_vector(m, state, XGPU_RT_BOUNCE_BLOCK_UNPROJECT + 1)), offset);
		unsigned int c = fdot(m, xyz(m, rt_bounce_vector(m, state, XGPU_RT_BOUNCE_BLOCK_UNPROJECT + 2)), offset);
		unsigned int corners = world_ao_pass_vector(m, &state->pass, WORLD_AO_PASS_CORNERS, 0), low, size, screen;

		low = op1(m, OP_CONVERT_S_TO_F, m->t_v2, op(m, OP_VECTOR_SHUFFLE, m->t_iv2, 4, (unsigned int[]){ corners, corners, 0, 1 }));
		size = fsub(m, 2, op1(m, OP_CONVERT_S_TO_F, m->t_v2, op(m, OP_VECTOR_SHUFFLE, m->t_iv2, 4,
			(unsigned int[]){ corners, corners, 2, 3 })), low);
		screen = construct(m, 2, (unsigned int[]){ fdiv(m, 1, b, a), fdiv(m, 1, c, a) });
		screen = fadd(m, 2, low, fmul(m, 2, fscale(m, 2, fadd(m, 2, screen, constant_splat(m, 2, 1.0f)),
			constant_float(m, 0.5f)), size));
		screen = ext1(m, GLSL_F_ABS, m->t_v2, fsub(m, 2, screen, op1(m, OP_CONVERT_S_TO_F, m->t_v2, pixel)));
		value = construct(m, 3, (unsigned int[]){ fmul(m, 1, extract(m, screen, 0), constant_float(m, 0.005f)),
			fmul(m, 1, extract(m, screen, 1), constant_float(m, 0.005f)), constant_float(m, 0.0f) });
	}
	else if (rt_bounce_view() == 4)
		value = construct(m, 3, (unsigned int[]){ constant_float(m, 0.0f), saturate(m, 1, fmul(m, 1, extract(m,
			light_shadows_fetch(m, state->pass.history, pixel), 3), constant_float(m, 0.1f))), constant_float(m, 0.0f) });
	else if (rt_bounce_view() == 3)
		value = fadd(m, 3, value, construct(m, 3, (unsigned int[]){ constant_float(m, 0.0f),
			saturate(m, 1, fmul(m, 1, total, constant_float(m, 0.5f))), constant_float(m, 0.0f) }));
	store(m, out, vec4_of(m, value, constant_float(m, 1.0f)));
	selection_end(m, merge);
}

BOOL xgpu_spirv_rt_bounce_pass(int which, const struct xgpu_spirv_options *options, struct xgpu_spirv_module *module)
{
	struct module m;
	struct words locals = { 0 };
	struct rt_bounce_state state;
	unsigned int main_id, out;
	BOOL result;

	if (which == XGPU_SPIRV_RT_BOUNCE_TRACE && !options->ray_queries)
		return FALSE;
	module_begin(&m, EXECUTION_MODEL_FRAGMENT);
	memset(&state, 0, sizeof(state));
	world_ao_declare(&m);
	world_ao_pass_declare(&m, &state.pass, which == XGPU_SPIRV_RT_BOUNCE_TRACE);
	state.block = rt_shadows_block(&m, XGPU_RT_BOUNCE_BLOCK, XGPU_SPIRV_BINDING_CONSTANTS, "rt_bounce_block");
	if (which == XGPU_SPIRV_RT_BOUNCE_RESOLVE || which == XGPU_SPIRV_RT_BOUNCE_GATHER)
	{
		state.samples = variable(&m, type_sampled_image(&m, DIM_2D, FALSE, FALSE, TRUE), STORAGE_UNIFORM_CONSTANT,
			"rt_bounce_samples");
		decorate1(&m, state.samples, DECORATION_DESCRIPTOR_SET, XGPU_SPIRV_SET_TEXTURES);
		decorate1(&m, state.samples, DECORATION_BINDING, XGPU_SPIRV_RT_BOUNCE_SAMPLES);
	}
	out = interface_variable(&m, m.t_v4, STORAGE_OUTPUT, 0, "rt_bounce");
	main_id = function_begin(&m);
	if (which == XGPU_SPIRV_RT_BOUNCE_TRACE)
		rt_bounce_trace_main(&m, &locals, &state, out);
	else if (which == XGPU_SPIRV_RT_BOUNCE_RESOLVE)
		rt_bounce_resolve_main(&m, &locals, &state, out);
	else
		rt_bounce_gather_main(&m, &locals, &state, out);
	result = module_end(&m, EXECUTION_MODEL_FRAGMENT, main_id, &locals, module);
	free(locals.words);
	return result;
}

/* the surfaces found once a pixel: the window's bounced light (the
occlusion set's XGPU_SPIRV_BINDING_RT_BOUNCE, a 1x1 of none where none is
traced) */
static void rt_bounce_declare(struct module *m, struct pixel_state *state)
{
	state->rt_bounce = variable(m, type_sampled_image(m, DIM_2D, FALSE, FALSE, TRUE), STORAGE_UNIFORM_CONSTANT,
		"rt_bounce");
	decorate1(m, state->rt_bounce, DECORATION_DESCRIPTOR_SET, XGPU_SPIRV_SET_OCCLUSION);
	decorate1(m, state->rt_bounce, DECORATION_BINDING, XGPU_SPIRV_BINDING_RT_BOUNCE);
}

/* ... added to light (linear, the enhanced lights' units), as GLSL:
light + texelFetch(rt_bounce, min(ivec2(gl_FragCoord.xy), textureSize(rt_bounce, 0) - 1), 0).rgb */
/* display.rt_bounce's binding's texel at the pixel (a vec4) */
static unsigned int rt_bounce_texel(struct module *m, struct pixel_state *state)
{
	unsigned int sampled, image, last, at, operands[4];

	sampled = load(m, type_sampled_image(m, DIM_2D, FALSE, FALSE, TRUE), state->rt_bounce);
	image = op(m, OP_IMAGE, type_sampled_image(m, DIM_2D, FALSE, FALSE, FALSE), 1, &sampled);
	last = op2(m, OP_I_SUB, m->t_iv2, image_size(m, image, m->t_iv2), rt_bounce_iv2(m, constant_int(m, 1), constant_int(m, 1)));
	at = ext2(m, GLSL_S_MIN, m->t_iv2, op1(m, OP_CONVERT_F_TO_S, m->t_iv2, xy(m, load(m, m->t_v4, state->frag_coord))), last);
	operands[0] = image;
	operands[1] = at;
	operands[2] = IMAGE_OPERANDS_LOD;
	operands[3] = constant_int(m, 0);
	return op(m, OP_IMAGE_FETCH, m->t_v4, 4, operands);
}

static unsigned int rt_bounce_add(struct module *m, struct pixel_state *state, unsigned int light)
{
	if (!state->rt_bounce || !state->frag_coord)
		return light;
	return fadd(m, 3, light, xyz(m, rt_bounce_texel(m, state)));
}

/* HALO_LT_SHINE_VIEW (the one light model's shine, lt_ggx, alone in the
reflection's texel: checking), read once */
static int lt_shine_view(void)
{
	static int on = -1;

	if (on < 0)
		on = getenv("HALO_LT_SHINE_VIEW") && atoi(getenv("HALO_LT_SHINE_VIEW")) == 1;
	return on;
}

/* HALO_LT_SUN_CLAMP_VIEW (lt_e1_add), read once */
static int lt_sun_clamp_view(void)
{
	static int on = -1;

	if (on < 0)
		on = getenv("HALO_LT_SUN_CLAMP_VIEW") && atoi(getenv("HALO_LT_SUN_CLAMP_VIEW")) == 1;
	return on;
}

static int lt_e1_on(void)
{
	return lt_shader_on;
}

/* the ray-traced lighting's light model (xgpu.h) for the shader being made:
with the pass alone; else the classic one, as before it */
static int lt_light_model(void)
{
	return lt_shader_on;
}

/* an octahedral normal, packSnorm2x16 (world_ao_inputs' folding) */
static unsigned int lt_octahedral(struct module *m, unsigned int n)
{
	unsigned int absolute, folded, flipped, positive, negative;

	n = ext1(m, GLSL_NORMALIZE, m->t_v3, n);
	absolute = ext1(m, GLSL_F_ABS, m->t_v3, n);
	folded = fdiv(m, 2, xy(m, n), splat(m, 2, fmax_scalar(m, 1, fadd(m, 1, fadd(m, 1, extract(m, absolute, 0),
		extract(m, absolute, 1)), extract(m, absolute, 2)), 1.0e-24f)));
	positive = op2(m, OP_F_ORD_GREATER_THAN_EQUAL, m->t_bv2, folded, constant_splat(m, 2, 0.0f));
	flipped = fmul(m, 2, fsub(m, 2, constant_splat(m, 2, 1.0f), ext1(m, GLSL_F_ABS, m->t_v2,
		op(m, OP_VECTOR_SHUFFLE, m->t_v2, 4, (unsigned int[]){ folded, folded, 1, 0 }))),
		op3(m, OP_SELECT, m->t_v2, positive, constant_splat(m, 2, 1.0f), constant_splat(m, 2, -1.0f)));
	negative = compare(m, OP_F_ORD_LESS_THAN, extract(m, n, 2), constant_float(m, 0.0f));
	return ext1(m, GLSL_PACK_SNORM_2X16, m->t_uint, select_n(m, 2, negative, flipped, folded));
}

/* a twin's motion (xgpu_spirv.h's C1): its positions as motion_screen has
them, this frame's and the frame before's, the one less the other as a
fraction of the window, x right and y down, in fixed point; NONE where
either is behind the eye or it moved more than the range */
static unsigned int lt_guides_motion(struct module *m, unsigned int current_in, unsigned int previous_in)
{
	static const float half_down[2] = { 0.5f / XGPU_SPIRV_GUIDES_MOTION_RANGE, -0.5f / XGPU_SPIRV_GUIDES_MOTION_RANGE };
	unsigned int current = load(m, m->t_v3, current_in), previous = load(m, m->t_v3, previous_in);
	unsigned int current_w = extract(m, current, 2), previous_w = extract(m, previous, 2);
	unsigned int now = fdiv(m, 2, xy(m, current), splat(m, 2, current_w));
	unsigned int before = fdiv(m, 2, xy(m, previous), splat(m, 2, previous_w));
	unsigned int moved, packed, kept, largest;

	moved = fmul(m, 2, fsub(m, 2, now, before), constant_vector(m, 2, half_down));
	packed = op2(m, OP_BITWISE_XOR, m->t_uint, ext1(m, GLSL_PACK_SNORM_2X16, m->t_uint, moved),
		constant_uint(m, XGPU_SPIRV_GUIDES_MOTION_FLIP));
	moved = ext1(m, GLSL_F_ABS, m->t_v2, moved);
	largest = ext2(m, GLSL_F_MAX, m->t_float, extract(m, moved, 0), extract(m, moved, 1));
	/* (both in front of the eyes, within the range: not NaN) */
	kept = op2(m, OP_LOGICAL_AND, m->t_bool, op2(m, OP_LOGICAL_AND, m->t_bool,
		compare(m, OP_F_ORD_GREATER_THAN, current_w, constant_float(m, 0.0f)),
		compare(m, OP_F_ORD_GREATER_THAN, previous_w, constant_float(m, 0.0f))),
		compare(m, OP_F_ORD_LESS_THAN_EQUAL, largest, constant_float(m, 1.0f)));
	return op3(m, OP_SELECT, m->t_uint, kept, packed, constant_uint(m, XGPU_SPIRV_GUIDES_MOTION_NONE));
}

/* a twin's guides (xgpu_spirv.h's C1): the shaded normal (0: the
interpolated one), the interpolated one, the material word (no id yet; the
roughness where the draw has one; the specular mask), the motion
(lt_guides_motion, or NONE) */
static unsigned int lt_e1_guides(struct module *m, struct pixel_state *state, unsigned int shaded, BOOL specular,
	unsigned int motion)
{
	unsigned int normal = xyz(m, pixel_input(m, state, state->in_world_normal, m->t_v4)), roughness, material;

	roughness = state->roughness ? op1(m, OP_BITCAST, m->t_uint, op1(m, OP_CONVERT_F_TO_S, m->t_int, ext1(m, GLSL_FLOOR,
		m->t_float, fadd(m, 1, fmul(m, 1, saturate(m, 1, state->roughness), constant_float(m, 255.0f)), constant_float(m, 0.5f))))) :
		constant_uint(m, XGPU_SPIRV_GUIDES_ROUGHNESS_NONE);
	material = op2(m, OP_BITWISE_OR, m->t_uint, op2(m, OP_SHIFT_LEFT_LOGICAL, m->t_uint, roughness,
		constant_uint(m, XGPU_SPIRV_GUIDES_ROUGHNESS_SHIFT)), constant_uint(m, specular ? 255u <<
		XGPU_SPIRV_GUIDES_SPECULAR_SHIFT : 0u));
	return op(m, OP_COMPOSITE_CONSTRUCT, m->t_uv4, 4, (unsigned int[]){ lt_octahedral(m, shaded ? shaded : normal),
		lt_octahedral(m, normal), material, motion });
}

/* ... the traced lights' light (linear, at display.rt_bounce's binding:
their irradiance, its cosine by the shaded normal of the twins' guides,
lt_e1_guides) added to lit (the game's lights' result, gamma) linearly, by
tint (linear; 0: by 1, a model's), after the game's own lights' shoulder;
the light less than 0 on the structure where a dynamic object shadows the
sun (the suns' share taken off: lt/e1.frag), as GLSL:
pow(max(pow(max(lit, 0.0), vec3(2.2)) + light * tint, 0.0), vec3(1.0 / 2.2)); lit as it
is without the mode or the binding */
static unsigned int lt_e1_add(struct module *m, struct pixel_state *state, unsigned int lit, unsigned int tint,
	unsigned int p, unsigned int n)
{
	unsigned int light, to_lamp, texel, base;

	if (!lt_e1_on() || !state->rt_bounce || !state->frag_coord)
		return lit;
	texel = rt_bounce_texel(m, state);
	light = xyz(m, texel);
	/* (the pass's light has the receiver's cosine, by the guides' shaded
	normal: lt_e1_guides) */
	(void)p;
	(void)n;
	(void)to_lamp;
	/* (a model's (no tint) never less than 0: the suns' share is the
	structure's, whose pixel a model the pre-pass did not draw reads) */
	if (tint)
		light = fmul(m, 3, light, tint);
	else
		light = fmax_scalar(m, 3, light, 0.0f);
	(void)texel;
	base = fadd(m, 3, ext2(m, GLSL_POW, m->t_v3, fmax_scalar(m, 3, lit, 0.0f), constant_splat(m, 3, 2.2f)), light);
	/* (HALO_LT_SUN_CLAMP_VIEW=1, checking: magenta where the sum is less
	than 0 in a channel, the suns' share taken off more than the lightmap's
	light has) */
	if (lt_sun_clamp_view())
	{
		unsigned int below = op1(m, OP_ANY, m->t_bool, op2(m, OP_F_ORD_LESS_THAN, m->t_bv3, base,
			constant_splat(m, 3, 0.0f)));

		return select_n(m, 3, below, constant_vector(m, 3, (const float[]){ 1.0f, 0.0f, 1.0f }),
			ext2(m, GLSL_POW, m->t_v3, fmax_scalar(m, 3, base, 0.0f), constant_splat(m, 3, 1.0f / 2.2f)));
	}
	/* (less than 0 where the suns' share a dynamic object shadows is more
	than the lightmap's light: kept at 0) */
	return ext2(m, GLSL_POW, m->t_v3, fmax_scalar(m, 3, base, 0.0f), constant_splat(m, 3, 1.0f / 2.2f));
}

/* ---------- the one light model's forward receivers (xgpu_spirv.h's
XGPU_SPIRV_BINDING_LT_TABLE; ~/dev/rt2-realtime-design.md, "One light model
for every receiver")

A draw lit by the enhanced dynamic lights that reads no pass light of its
own (state->lt_forward: the first-person weapon and hands, a blended or
self-illuminated model's or structure's pass) lights itself by the pass's
own direct light, from the pass's light table, lt/e1.frag's formula
(light_estimate's, in colour) at each light's emitter middle, with one
visibility ray to it, as GLSL:

vec3 sum = vec3(0.0);
for each row of the pixel's cell (the dynamic lights' grid, as the pass's
  cell_lights), then the view's flashlight's row (FLASHLIGHT_ROW):
  vec4 t0..t6 = the row's texels; if (t4.w < 0.0 || t4.w > 2.5) continue;   (not the pass's; a sun)
  vec3 to = t0.xyz - p; float d2 = max(dot(to, to), 1e-8), d = sqrt(d2); vec3 l = to / d;
  float window = range_window(d, t1.w), e = dot(t1.xyz, -l), r2 = t0.w * t0.w, profile;
  point: profile = 1.0 / max(d2, r2);
  spot:  profile = e > 0 ? cone_ramp(e, t2.w, t3.w) * e / max(d2, 1e-4) : 0, by the gel's
         pow(cookie(a, b), 2.2) where it has one (t5.y >= 0, t5.z its half width; from t6.xyz
         where t6.w is 1, else from t0.xyz);
  panel: profile = e > 0 ? e * r2 / (r2 + d2) : 0;
  float lit = dot(n, l) > 0.0 ? profile * max(dot(shaded, l), 0.0) * window : 0.0;
  if (lit > 0.0) lit *= visible(origin, t0.xyz, t_max = |t0.xyz - origin| - max(1.5 t0.w, 0.1),
                                its owner int(t5.x), the draw's own instance);
  sum += t4.rgb * lit;
origin: the first-person weapon's (the draw's object WORLD_AO_OWNER_UNLIT) the eye (grid[0].xyz): it is
drawn over the world from the viewer's place, so the world shadows it only where it shadows the eye;
else p + n * (0.004 + 0.0005 |p - eye|), the pass's offset.
The first-person weapon also leaves out the lights within its reach of
the eye that the game did not place on the view model (the forward table's
texel 7's y): those are its holder's third-person body's and held weapon's
(an ammo counter's glow), which the view model stands in for, not where it
is drawn; a model's own lights (model_lights[XGPU_MODEL_LIGHT_OWN_LIGHTS])
light it no more than in the classic sum.

The pass's lights the table's rows carry (t4.w >= 0) and the view's
flashlight (its block row XGPU_SPIRV_LT_KIND_FLASHLIGHT) are then left out
of the draw's classic sum (dynamic_light_sum's forward), which keeps the
models-only lights (XGPU_SPIRV_LT_KIND_CLASSIC) as the pass's receivers keep
them. Linear irradiance, the pass's units: lt_forward_add adds it as
lt_e1_add adds the pass's. */

#define LT_FORWARD_OWNER_FIRST_PERSON (-2.0f)
/* how far from the eye a first-person model reaches at most (world units:
d3d8_device.c's MODEL_BOUNDS_FIRST_PERSON_REACH) */
#define LT_FORWARD_FIRST_PERSON_REACH 1.5f

static void lt_table_declare(struct module *m, struct pixel_state *state)
{
	state->lt_table = variable(m, type_sampled_image(m, DIM_2D, FALSE, FALSE, TRUE), STORAGE_UNIFORM_CONSTANT, "lt_table");
	decorate1(m, state->lt_table, DECORATION_DESCRIPTOR_SET, XGPU_SPIRV_SET_OCCLUSION);
	decorate1(m, state->lt_table, DECORATION_BINDING, XGPU_SPIRV_BINDING_LT_TABLE);
	if (!m->rt_reflection_scene)
	{
		m->rt_reflection_scene = variable(m, type0(m, OP_TYPE_ACCELERATION_STRUCTURE), STORAGE_UNIFORM_CONSTANT,
			"rt_reflection_scene");
		decorate1(m, m->rt_reflection_scene, DECORATION_DESCRIPTOR_SET, XGPU_SPIRV_SET_OCCLUSION);
		decorate1(m, m->rt_reflection_scene, DECORATION_BINDING, XGPU_SPIRV_BINDING_WORLD_AO_SCENE);
		m->ray_query = TRUE;
	}
	rt_hits_declare(m);
}

/* the table as an image (texelFetch's) */
static unsigned int lt_table_image(struct module *m, struct pixel_state *state)
{
	unsigned int sampled = load(m, type_sampled_image(m, DIM_2D, FALSE, FALSE, TRUE), state->lt_table);

	return op(m, OP_IMAGE, type_sampled_image(m, DIM_2D, FALSE, FALSE, FALSE), 1, &sampled);
}

/* a row's (an int) texel (a vec4) */
static unsigned int lt_table_texel(struct module *m, unsigned int image, unsigned int row, int texel)
{
	unsigned int operands[4];

	operands[0] = image;
	operands[1] = op(m, OP_COMPOSITE_CONSTRUCT, m->t_iv2, 2, (unsigned int[]){ constant_int(m, texel), row });
	operands[2] = IMAGE_OPERANDS_LOD;
	operands[3] = constant_int(m, 0);
	return op(m, OP_IMAGE_FETCH, m->t_v4, 4, operands);
}

/* the rows the bound table has (the stand-in's 1: none read) */
static unsigned int lt_table_rows(struct module *m, unsigned int image)
{
	return op2(m, OP_COMPOSITE_EXTRACT, m->t_int, image_size(m, image, m->t_iv2), 1);
}

/* whether the segment from origin toward target is clear short of the
emitter (its radius, as the pass ends its rays: lt/e1.frag's light_sample),
its owner's instance and the receiver's own left out, the structure never:
1 or 0 */
static unsigned int lt_forward_visible(struct module *m, struct pixel_state *state, unsigned int origin,
	unsigned int target, unsigned int radius, unsigned int owner, unsigned int receiver)
{
	struct words *locals = &state->locals;
	unsigned int query = local(m, locals, type0(m, OP_TYPE_RAY_QUERY)), seen = local(m, locals, m->t_float);
	unsigned int candidate = constant_int(m, RAY_QUERY_CANDIDATE), committed = constant_int(m, RAY_QUERY_COMMITTED);
	unsigned int ray = fsub(m, 3, target, origin), length = flength(m, ray), d, t_max, operands[8], merge;
	unsigned int header = id_new(m), condition = id_new(m), body = id_new(m), next = id_new(m), loop_merge = id_new(m);
	unsigned int custom, structure, other, mine_merge;

	store(m, seen, constant_float(m, 1.0f));
	d = fdiv(m, 3, ray, splat(m, 3, fmax_scalar(m, 1, length, 1.0e-6f)));
	t_max = fsub(m, 1, length, fmax_scalar(m, 1, fmul(m, 1, radius, constant_float(m, 1.5f)), 0.1f));
	selection_begin(m, compare(m, OP_F_ORD_GREATER_THAN, t_max, constant_float(m, 0.0f)), &merge);
	operands[0] = query;
	operands[1] = load(m, type0(m, OP_TYPE_ACCELERATION_STRUCTURE), m->rt_reflection_scene);
	operands[2] = constant_uint(m, 0);
	operands[3] = constant_uint(m, XGPU_SPIRV_RT_MASK_AO);
	operands[4] = origin;
	operands[5] = constant_float(m, 0.0f);
	operands[6] = d;
	operands[7] = t_max;
	op_void(m, OP_RAY_QUERY_INITIALIZE, 8, operands);
	branch(m, header);
	label_at(m, header);
	operands[0] = loop_merge;
	operands[1] = next;
	operands[2] = 0;
	op_void(m, OP_LOOP_MERGE, 3, operands);
	branch(m, condition);
	label_at(m, condition);
	branch_conditional(m, op1(m, OP_RAY_QUERY_PROCEED, m->t_bool, query), body, loop_merge);
	label_at(m, body);
	custom = op2(m, OP_RAY_QUERY_GET_INTERSECTION_INSTANCE_CUSTOM_INDEX, m->t_int, query, candidate);
	structure = op2(m, OP_I_EQUAL, m->t_bool, custom, constant_int(m, 0));
	other = op2(m, OP_LOGICAL_AND, m->t_bool, op2(m, OP_I_NOT_EQUAL, m->t_bool, custom, owner),
		op2(m, OP_I_NOT_EQUAL, m->t_bool, custom, receiver));
	selection_begin(m, rt_candidate_commits(m, locals, query, structure, other), &mine_merge);
	op_void(m, OP_RAY_QUERY_CONFIRM_INTERSECTION, 1, &query);
	selection_end(m, mine_merge);
	branch(m, next);
	label_at(m, next);
	branch(m, header);
	label_at(m, loop_merge);
	store(m, seen, fselect(m, op2(m, OP_I_EQUAL, m->t_bool, op2(m, OP_RAY_QUERY_GET_INTERSECTION_TYPE, m->t_uint, query,
		committed), constant_uint(m, 0)), constant_float(m, 1.0f), constant_float(m, 0.0f)));
	selection_end(m, merge);
	return load(m, m->t_float, seen);
}

/* the flashlight's cookie (the cells' XGPU_DYNAMIC_LIGHTS_FLASHLIGHT_COOKIE,
as lt/e1.frag's cookie reads it) at a, b in -1 to 1, bilinear, each axis
warped (u = sign(a) sqrt(|a|)): gamma, 0 to 1 */
static unsigned int lt_forward_cookie(struct module *m, unsigned int a, unsigned int b)
{
	struct flashlight_bilinear bl;
	unsigned int x, y;
	const float size = (float)XGPU_DYNAMIC_LIGHTS_FLASHLIGHT_COOKIE_SIZE;

	x = fmul(m, 1, ext1(m, GLSL_F_SIGN, m->t_float, a), fsqrt(m, ext1(m, GLSL_F_ABS, m->t_float, a)));
	y = fmul(m, 1, ext1(m, GLSL_F_SIGN, m->t_float, b), fsqrt(m, ext1(m, GLSL_F_ABS, m->t_float, b)));
	x = fsub(m, 1, fmul(m, 1, fadd(m, 1, fmul(m, 1, x, constant_float(m, 0.5f)), constant_float(m, 0.5f)),
		constant_float(m, size)), constant_float(m, 0.5f));
	y = fsub(m, 1, fmul(m, 1, fadd(m, 1, fmul(m, 1, y, constant_float(m, 0.5f)), constant_float(m, 0.5f)),
		constant_float(m, size)), constant_float(m, 0.5f));
	flashlight_bilinear(m, x, y, XGPU_DYNAMIC_LIGHTS_FLASHLIGHT_COOKIE_SIZE, &bl);
	return flashlight_mix(m, flashlight_mix(m, flashlight_cookie_byte(m, bl.x0, bl.y0),
		flashlight_cookie_byte(m, bl.x1, bl.y0), bl.fx), flashlight_mix(m, flashlight_cookie_byte(m, bl.x0, bl.y1),
		flashlight_cookie_byte(m, bl.x1, bl.y1), bl.fx), bl.fy);
}

/* what lt_forward_light reads once a pixel; with spec (a vec3 variable, 0
none), each light's GGX highlight (lt_ggx) summed there as well, by the
pixel's to-eye vector v and the material's alpha (roughness squared) */
struct lt_forward_read
{
	unsigned int image, rows, p, n, shaded, origin, receiver, sum, eye, first_person;
	unsigned int spec, v, alpha;
};

/* ---------- the one light model's shine (~/dev/rt2-realtime-design.md
section 10): GGX's specular lobe for a light from direction l, as the
surfaces' reflection texel takes it (Halo's combiners after it supply the
Fresnel term: the shader's reflection strength, tint and mask), in the
pass's units (its irradiance is the true one over pi), as GLSL:

vec3 h = normalize(l + v); float nl = max(dot(n, l), 0.0), nv = max(dot(n, v), 1e-4), nh = max(dot(n, h), 0.0);
float a2 = alpha * alpha, q = nh * nh * (a2 - 1.0) + 1.0;
float vis = 0.5 / (nl * sqrt(nv * nv * (1.0 - a2) + a2) + nv * sqrt(nl * nl * (1.0 - a2) + a2) + 1e-7);
return a2 / max(q * q, 1e-12) * vis;     (pi D Vis: Smith's height-correlated visibility)

alpha is widened by the emitter's angular radius (Karis's sphere-light
normalization, alpha + radius / (2 distance)) so a small light on a smooth
surface is a highlight of the light's size, not a speck that aliases. */
static unsigned int lt_ggx(struct module *m, unsigned int n, unsigned int v, unsigned int l, unsigned int alpha)
{
	unsigned int h = ext1(m, GLSL_NORMALIZE, m->t_v3, fadd(m, 3, l, v));
	unsigned int nl = fmax_scalar(m, 1, fdot(m, n, l), 0.0f), nv = fmax_scalar(m, 1, fdot(m, n, v), 1.0e-4f);
	unsigned int nh = fmax_scalar(m, 1, fdot(m, n, h), 0.0f), a2 = fmul(m, 1, alpha, alpha);
	unsigned int one_less = fsub(m, 1, constant_float(m, 1.0f), a2), q, vis;

	q = fadd(m, 1, fmul(m, 1, fmul(m, 1, nh, nh), fsub(m, 1, a2, constant_float(m, 1.0f))), constant_float(m, 1.0f));
	vis = fdiv(m, 1, constant_float(m, 0.5f), fadd(m, 1, fadd(m, 1,
		fmul(m, 1, nl, fsqrt(m, fadd(m, 1, fmul(m, 1, fmul(m, 1, nv, nv), one_less), a2))),
		fmul(m, 1, nv, fsqrt(m, fadd(m, 1, fmul(m, 1, fmul(m, 1, nl, nl), one_less), a2)))), constant_float(m, 1.0e-7f)));
	return fmul(m, 1, fdiv(m, 1, a2, fmax_scalar(m, 1, fmul(m, 1, q, q), 1.0e-12f)), vis);
}

/* alpha widened by an emitter of radius r at distance d (lt_ggx), at most 1 */
static unsigned int lt_ggx_widened(struct module *m, unsigned int alpha, unsigned int r, unsigned int d)
{
	return ext2(m, GLSL_F_MIN, m->t_float, fadd(m, 1, alpha, fdiv(m, 1, fmul(m, 1, r, constant_float(m, 0.5f)),
		fmax_scalar(m, 1, d, 1.0e-3f))), constant_float(m, 1.0f));
}

/* a row's (an int) light at the pixel, added to the sum (lt_forward_light's
loop body) */
static void lt_forward_row(struct module *m, struct pixel_state *state, const struct lt_forward_read *read,
	unsigned int row)
{
	unsigned int t[7], kind, to, d2, d, l, window, x, e, r2, ramp, inner, outer, spot, panel, point, profile;
	unsigned int gel_origin, v, z, inverse, a, b, inside, gel, has_gel, lit, seen, merge, lit_merge, taken;
	int texel;

	taken = op2(m, OP_S_LESS_THAN, m->t_bool, row, read->rows);
	selection_begin(m, taken, &merge);
	t[4] = lt_table_texel(m, read->image, row, 4);
	t[0] = lt_table_texel(m, read->image, row, 0);
	kind = extract(m, t[4], 3);
	/* (the first-person weapon: not the lights of its holder's body and
	held weapon, which its view model stands in for (within the
	first-person reach of the eye) but those the game placed on the view
	model itself (texel 7's y): the third-person body's lights are not
	where the view model is drawn) */
	{
		unsigned int placed = compare(m, OP_F_ORD_GREATER_THAN, extract(m, lt_table_texel(m, read->image, row, 7), 1),
			constant_float(m, 0.5f));
		unsigned int near = compare(m, OP_F_ORD_LESS_THAN, flength(m, fsub(m, 3, xyz(m, t[0]), read->eye)),
			constant_float(m, LT_FORWARD_FIRST_PERSON_REACH));
		unsigned int holders = op2(m, OP_LOGICAL_AND, m->t_bool, read->first_person, op2(m, OP_LOGICAL_AND, m->t_bool,
			near, op1(m, OP_LOGICAL_NOT, m->t_bool, placed)));

		selection_begin(m, op2(m, OP_LOGICAL_AND, m->t_bool, op2(m, OP_LOGICAL_AND, m->t_bool,
			compare(m, OP_F_ORD_GREATER_THAN_EQUAL, kind, constant_float(m, 0.0f)),
			compare(m, OP_F_ORD_LESS_THAN, kind, constant_float(m, 2.5f))), op1(m, OP_LOGICAL_NOT, m->t_bool, holders)),
			&lit_merge);
	}
	for (texel = 1; texel < 7; texel++)
		if (texel != 4)
			t[texel] = lt_table_texel(m, read->image, row, texel);
	to = fsub(m, 3, xyz(m, t[0]), read->p);
	d2 = fmax_scalar(m, 1, fdot(m, to, to), 1.0e-8f);
	d = fsqrt(m, d2);
	l = fdiv(m, 3, to, splat(m, 3, d));
	/* (the range window (1 - (d / r)^4)^2) */
	x = fmul(m, 1, d, extract(m, t[1], 3));
	window = saturate(m, 1, fsub(m, 1, constant_float(m, 1.0f), fmul(m, 1, fmul(m, 1, x, x), fmul(m, 1, x, x))));
	window = fmul(m, 1, window, window);
	e = fdot(m, xyz(m, t[1]), fneg(m, 3, l));
	r2 = fmul(m, 1, extract(m, t[0], 3), extract(m, t[0], 3));
	/* (a spot: its cone's ramp (none: outer -1; a step where inner <= outer), e / d^2, by its gel) */
	inner = extract(m, t[2], 3);
	outer = extract(m, t[3], 3);
	ramp = saturate(m, 1, fdiv(m, 1, fsub(m, 1, e, outer), fmax_scalar(m, 1, fsub(m, 1, inner, outer), 1.0e-6f)));
	ramp = fselect(m, compare(m, OP_F_ORD_LESS_THAN_EQUAL, inner, outer), fselect(m, compare(m, OP_F_ORD_GREATER_THAN, e,
		outer), constant_float(m, 1.0f), constant_float(m, 0.0f)), ramp);
	ramp = fselect(m, compare(m, OP_F_ORD_LESS_THAN_EQUAL, outer, constant_float(m, -1.0f)), constant_float(m, 1.0f), ramp);
	spot = fdiv(m, 1, fmul(m, 1, ramp, e), fmax_scalar(m, 1, d2, 1.0e-4f));
	gel_origin = select_n(m, 3, compare(m, OP_F_ORD_GREATER_THAN, extract(m, t[6], 3), constant_float(m, 0.5f)),
		xyz(m, t[6]), xyz(m, t[0]));
	v = fsub(m, 3, read->p, gel_origin);
	z = fdot(m, v, xyz(m, t[1]));
	inverse = fdiv(m, 1, constant_float(m, 1.0f), fmul(m, 1, fmax_scalar(m, 1, z, 1.0e-4f),
		fmax_scalar(m, 1, extract(m, t[5], 2), 1.0e-4f)));
	a = fmul(m, 1, fdot(m, v, xyz(m, t[3])), inverse);
	b = fmul(m, 1, fdot(m, v, xyz(m, t[2])), inverse);
	inside = op2(m, OP_LOGICAL_AND, m->t_bool, compare(m, OP_F_ORD_GREATER_THAN, z, constant_float(m, 1.0e-4f)),
		op2(m, OP_LOGICAL_AND, m->t_bool,
		compare(m, OP_F_ORD_LESS_THAN, ext1(m, GLSL_F_ABS, m->t_float, a), constant_float(m, 1.0f)),
		compare(m, OP_F_ORD_LESS_THAN, ext1(m, GLSL_F_ABS, m->t_float, b), constant_float(m, 1.0f))));
	gel = fselect(m, inside, ext2(m, GLSL_POW, m->t_float, fmax_scalar(m, 1, lt_forward_cookie(m,
		fclamp_scalar(m, a, -1.0f, 1.0f), fclamp_scalar(m, b, -1.0f, 1.0f)), 0.0f), constant_float(m, 2.2f)),
		constant_float(m, 0.0f));
	has_gel = op2(m, OP_LOGICAL_AND, m->t_bool, compare(m, OP_F_ORD_GREATER_THAN_EQUAL, extract(m, t[5], 1),
		constant_float(m, 0.0f)), compare(m, OP_F_ORD_GREATER_THAN, extract(m, t[5], 2), constant_float(m, 0.0f)));
	spot = fmul(m, 1, spot, fselect(m, has_gel, gel, constant_float(m, 1.0f)));
	/* (a panel: a one-sided Lambertian disk, e r^2 / (r^2 + d^2); a point: 1 / max(d^2, r^2)) */
	panel = fdiv(m, 1, fmul(m, 1, e, r2), fadd(m, 1, r2, d2));
	point = fdiv(m, 1, constant_float(m, 1.0f), ext2(m, GLSL_F_MAX, m->t_float, d2, r2));
	profile = fselect(m, compare(m, OP_F_ORD_LESS_THAN, kind, constant_float(m, 0.5f)), point,
		fselect(m, compare(m, OP_F_ORD_LESS_THAN, e, constant_float(m, 0.0f)), constant_float(m, 0.0f),
		fselect(m, compare(m, OP_F_ORD_LESS_THAN, kind, constant_float(m, 1.5f)), spot, panel)));
	/* (the receiver's cosine by its shaded normal; the side lit by its geometric one) */
	lit = fmul(m, 1, fmul(m, 1, profile, fmax_scalar(m, 1, fdot(m, read->shaded, l), 0.0f)), window);
	lit = fselect(m, compare(m, OP_F_ORD_GREATER_THAN, fdot(m, read->n, l), constant_float(m, 0.0f)), lit,
		constant_float(m, 0.0f));
	{
		unsigned int ray_merge, lit_variable = local(m, &state->locals, m->t_float);

		store(m, lit_variable, lit);
		selection_begin(m, compare(m, OP_F_ORD_GREATER_THAN, lit, constant_float(m, 0.0f)), &ray_merge);
		seen = lt_forward_visible(m, state, read->origin, xyz(m, t[0]), extract(m, t[0], 3),
			op1(m, OP_CONVERT_F_TO_S, m->t_int, extract(m, t[5], 0)), read->receiver);
		store(m, lit_variable, fmul(m, 1, lit, seen));
		selection_end(m, ray_merge);
		store(m, read->sum, fadd(m, 3, load(m, m->t_v3, read->sum), fscale(m, 3, xyz(m, t[4]),
			load(m, m->t_float, lit_variable))));
		/* (the shine: the same light, its visibility, by GGX's lobe toward the
		emitter's middle at the widened roughness) */
		if (read->spec)
			store(m, read->spec, fadd(m, 3, load(m, m->t_v3, read->spec), fscale(m, 3, xyz(m, t[4]),
				fmul(m, 1, load(m, m->t_float, lit_variable), lt_ggx(m, read->shaded, read->v, l,
				lt_ggx_widened(m, read->alpha, extract(m, t[0], 3), d))))));
	}
	selection_end(m, lit_merge);
	selection_end(m, merge);
}

/* the shine of a sun row (XGPU_SPIRV_LT_SUN_ROW + k) at the pixel, added
to read->spec: a sun narrower than LT_SUN_SPEC_WIDEST (b30's 0.2 degree
sun, not its 50 degree fill, whose light a traced reflection's misses
already show as the sky), its power by the receiver's cosine, one
visibility ray toward it as far as LT_SUN_REACH, GGX's lobe at the
roughness widened by its angular radius */
#define LT_SUN_SPEC_WIDEST 0.2f
#define LT_SUN_REACH 4096.0f
#define LT_FORWARD_SUN_ROWS 2

static void lt_forward_sun(struct module *m, struct pixel_state *state, const struct lt_forward_read *read, int k)
{
	unsigned int row = constant_int(m, XGPU_SPIRV_LT_SUN_ROW + k), t1, t4, t5, l, cosine, lit, merge, ray_merge;
	unsigned int lit_variable = local(m, &state->locals, m->t_float), taken, seen;

	taken = op2(m, OP_S_LESS_THAN, m->t_bool, row, read->rows);
	selection_begin(m, taken, &merge);
	t4 = lt_table_texel(m, read->image, row, 4);
	t1 = lt_table_texel(m, read->image, row, 1);
	t5 = lt_table_texel(m, read->image, row, 5);
	l = xyz(m, t1);
	cosine = fmax_scalar(m, 1, fdot(m, read->shaded, l), 0.0f);
	lit = fselect(m, op2(m, OP_LOGICAL_AND, m->t_bool, op2(m, OP_LOGICAL_AND, m->t_bool,
		compare(m, OP_F_ORD_GREATER_THAN, extract(m, t4, 3), constant_float(m, 2.5f)),
		compare(m, OP_F_ORD_LESS_THAN, extract(m, t5, 3), constant_float(m, LT_SUN_SPEC_WIDEST))),
		compare(m, OP_F_ORD_GREATER_THAN, fdot(m, read->n, l), constant_float(m, 0.0f))), cosine, constant_float(m, 0.0f));
	store(m, lit_variable, lit);
	selection_begin(m, compare(m, OP_F_ORD_GREATER_THAN, lit, constant_float(m, 0.0f)), &ray_merge);
	seen = lt_forward_visible(m, state, read->origin, fadd(m, 3, read->origin, fscale(m, 3, l,
		constant_float(m, LT_SUN_REACH))), constant_float(m, 0.0f), constant_int(m, -1), read->receiver);
	store(m, lit_variable, fmul(m, 1, lit, seen));
	selection_end(m, ray_merge);
	store(m, read->spec, fadd(m, 3, load(m, m->t_v3, read->spec), fscale(m, 3, xyz(m, t4),
		fmul(m, 1, load(m, m->t_float, lit_variable), lt_ggx(m, read->shaded, read->v, l,
		ext2(m, GLSL_F_MIN, m->t_float, fadd(m, 1, read->alpha, fmul(m, 1, extract(m, t5, 3), constant_float(m, 0.5f))),
		constant_float(m, 1.0f)))))));
	selection_end(m, merge);
}

/* the pass's direct light at p (n its geometric normal, shaded its shaded
one): linear irradiance; with state->lt_spec, the shine summed there too
(lt_forward_read's spec) */
static unsigned int lt_forward_light(struct module *m, struct pixel_state *state, unsigned int p, unsigned int n,
	unsigned int shaded, BOOL environment)
{
	unsigned int grid0 = dynamic_light(m, 0, constant_int(m, 0)), grid1 = dynamic_light(m, 0, constant_int(m, 1));
	unsigned int eye = xyz(m, grid0), count, object, first_person, offset, cell, cell_index, mask, operands[3];
	unsigned int word_variable, bits_variable, word, bits, light;
	unsigned int outer_header, outer_condition, outer_body, outer_continue, outer_merge;
	unsigned int inner_header, inner_condition, inner_body, inner_continue, inner_merge;
	struct lt_forward_read read;

	read.image = lt_table_image(m, state);
	read.rows = lt_table_rows(m, read.image);
	read.p = p;
	read.n = n;
	read.shaded = shaded;
	read.sum = local(m, &state->locals, m->t_v3);
	store(m, read.sum, constant_splat(m, 3, 0.0f));
	read.spec = state->lt_spec;
	read.v = state->lt_spec_v;
	read.alpha = state->lt_spec_alpha;
	/* (the draw's object: the first-person weapon's rays from the eye; its
	own instance among the rays', left out) */
	object = uniform(m, U_WORLD_AO, -1);
	first_person = compare(m, OP_F_ORD_LESS_THAN, ext1(m, GLSL_F_ABS, m->t_float, fsub(m, 1,
		extract(m, object, 0), constant_float(m, LT_FORWARD_OWNER_FIRST_PERSON))), constant_float(m, 0.5f));
	read.receiver = op1(m, OP_CONVERT_F_TO_S, m->t_int, extract(m, object, 2));
	read.eye = eye;
	read.first_person = first_person;
	read.origin = select_n(m, 3, first_person, eye, fadd(m, 3, p, fscale(m, 3, n, fadd(m, 1, constant_float(m, 0.004f),
		fmul(m, 1, constant_float(m, 0.0005f), flength(m, fsub(m, 3, p, eye)))))));
	count = op1(m, OP_CONVERT_F_TO_S, m->t_int, extract(m, grid0, 3));
	/* (the cell's lights: the pass's cell_lights) */
	offset = fsub(m, 3, p, eye);
	cell = fadd(m, 3, fmul(m, 3, fmul(m, 3, ext1(m, GLSL_F_SIGN, m->t_v3, offset),
		ext1(m, GLSL_LOG2, m->t_v3, fadd(m, 3, constant_splat(m, 3, 1.0f),
		fscale(m, 3, ext1(m, GLSL_F_ABS, m->t_v3, offset), extract(m, grid1, 3))))), xyz(m, grid1)),
		constant_vector(m, 3, (const float[]){ (float)(XGPU_DYNAMIC_LIGHTS_GRID_X / 2), (float)(XGPU_DYNAMIC_LIGHTS_GRID_Y / 2),
		(float)(XGPU_DYNAMIC_LIGHTS_GRID_Z / 2) }));
	cell_index = ext3(m, GLSL_S_CLAMP, m->t_iv3, op1(m, OP_CONVERT_F_TO_S, m->t_iv3, ext1(m, GLSL_FLOOR, m->t_v3, cell)),
		op(m, OP_COMPOSITE_CONSTRUCT, m->t_iv3, 3, (unsigned int[]){ constant_int(m, 0), constant_int(m, 0), constant_int(m, 0) }),
		op(m, OP_COMPOSITE_CONSTRUCT, m->t_iv3, 3, (unsigned int[]){ constant_int(m, XGPU_DYNAMIC_LIGHTS_GRID_X - 1),
		constant_int(m, XGPU_DYNAMIC_LIGHTS_GRID_Y - 1), constant_int(m, XGPU_DYNAMIC_LIGHTS_GRID_Z - 1) }));
	mask = dynamic_light_texel(m, cell_index);
	word_variable = local(m, &state->locals, m->t_int);
	bits_variable = local(m, &state->locals, m->t_uint);
	store(m, word_variable, constant_int(m, 0));
	outer_header = id_new(m);
	outer_condition = id_new(m);
	outer_body = id_new(m);
	outer_continue = id_new(m);
	outer_merge = id_new(m);
	branch(m, outer_header);
	label_at(m, outer_header);
	operands[0] = outer_merge;
	operands[1] = outer_continue;
	operands[2] = 0;
	op_void(m, OP_LOOP_MERGE, 3, operands);
	branch(m, outer_condition);
	label_at(m, outer_condition);
	word = load(m, m->t_int, word_variable);
	branch_conditional(m, op2(m, OP_S_LESS_THAN, m->t_bool, word, constant_int(m, 4)), outer_body, outer_merge);
	label_at(m, outer_body);
	store(m, bits_variable, op2(m, OP_VECTOR_EXTRACT_DYNAMIC, m->t_uint, mask, word));
	inner_header = id_new(m);
	inner_condition = id_new(m);
	inner_body = id_new(m);
	inner_continue = id_new(m);
	inner_merge = id_new(m);
	branch(m, inner_header);
	label_at(m, inner_header);
	operands[0] = inner_merge;
	operands[1] = inner_continue;
	operands[2] = 0;
	op_void(m, OP_LOOP_MERGE, 3, operands);
	branch(m, inner_condition);
	label_at(m, inner_condition);
	bits = load(m, m->t_uint, bits_variable);
	branch_conditional(m, op2(m, OP_I_NOT_EQUAL, m->t_bool, bits, constant_uint(m, 0)), inner_body, inner_merge);
	label_at(m, inner_body);
	light = op2(m, OP_I_ADD, m->t_int, op2(m, OP_I_MUL, m->t_int, word, constant_int(m, 32)),
		ext1(m, GLSL_FIND_I_LSB, m->t_int, bits));
	store(m, bits_variable, op2(m, OP_BITWISE_AND, m->t_uint, bits, op2(m, OP_I_SUB, m->t_uint, bits, constant_uint(m, 1))));
	{
		unsigned int merge;

		unsigned int taken = op2(m, OP_S_LESS_THAN, m->t_bool, light, count);

		/* (the block's lights alone: row < count; a model's but those its
		object's own, which do not light it, as the classic sum leaves
		them: its model_lights[XGPU_MODEL_LIGHT_OWN_LIGHTS]) */
		if (!environment)
		{
			unsigned int own = uniform(m, U_MODEL_LIGHTS, XGPU_MODEL_LIGHT_OWN_LIGHTS);
			unsigned int index = op1(m, OP_CONVERT_S_TO_F, m->t_float, light), mine;
			int slot;

			mine = compare(m, OP_F_ORD_EQUAL, extract(m, own, 0), index);
			for (slot = 1; slot < 4; slot++)
				mine = op2(m, OP_LOGICAL_OR, m->t_bool, mine, compare(m, OP_F_ORD_EQUAL, extract(m, own, slot), index));
			taken = op2(m, OP_LOGICAL_AND, m->t_bool, taken, op1(m, OP_LOGICAL_NOT, m->t_bool, mine));
		}
		selection_begin(m, taken, &merge);
		lt_forward_row(m, state, &read, light);
		selection_end(m, merge);
	}
	branch(m, inner_continue);
	label_at(m, inner_continue);
	branch(m, inner_header);
	label_at(m, inner_merge);
	branch(m, outer_continue);
	label_at(m, outer_continue);
	store(m, word_variable, op2(m, OP_I_ADD, m->t_int, load(m, m->t_int, word_variable), constant_int(m, 1)));
	branch(m, outer_header);
	label_at(m, outer_merge);
	/* (the view's shadowed flashlight, E1's, in its own row) */
	lt_forward_row(m, state, &read, constant_int(m, XGPU_SPIRV_LT_FLASHLIGHT_ROW));
	if (read.spec)
	{
		int k;

		for (k = 0; k < LT_FORWARD_SUN_ROWS; k++)
			lt_forward_sun(m, state, &read, k);
	}
	return load(m, m->t_v3, read.sum);
}

/* whether the classic sum leaves a light (an int, the block's index) out
for a forward receiver: the pass lights it (its row's kind 0 or more), or
it is the view's flashlight (its own row lights it): a bool */
static unsigned int lt_forward_skips(struct module *m, struct pixel_state *state, unsigned int light)
{
	unsigned int image = lt_table_image(m, state), kind;

	kind = extract(m, lt_table_texel(m, image, light, 4), 3);
	return op2(m, OP_LOGICAL_AND, m->t_bool, op2(m, OP_S_LESS_THAN, m->t_bool, light, lt_table_rows(m, image)),
		op2(m, OP_LOGICAL_OR, m->t_bool, compare(m, OP_F_ORD_GREATER_THAN_EQUAL, kind, constant_float(m, -0.5f)),
		compare(m, OP_F_ORD_LESS_THAN_EQUAL, kind, constant_float(m, -1.5f))));
}

/* the forward receiver's light (lt_forward_light) added to lit (the game's
lights' result, gamma) as lt_e1_add adds the pass's, by tint (linear; 0: by
1, a model's), as GLSL:
pow(max(pow(max(lit, 0.0), vec3(2.2)) + light * tint, 0.0), vec3(1.0 / 2.2)) */
static unsigned int lt_forward_add(struct module *m, struct pixel_state *state, unsigned int lit, unsigned int tint,
	unsigned int p, unsigned int n, unsigned int shaded)
{
	unsigned int light;

	if (!state->lt_forward)
		return lit;
	/* (tint: the structure's; none, a model's) */
	light = lt_forward_light(m, state, p, n, shaded, tint != 0);
	if (tint)
		light = fmul(m, 3, light, tint);
	return ext2(m, GLSL_POW, m->t_v3, fmax_scalar(m, 3, fadd(m, 3, ext2(m, GLSL_POW, m->t_v3, fmax_scalar(m, 3, lit, 0.0f),
		constant_splat(m, 3, 2.2f)), light), 0.0f), constant_splat(m, 3, 1.0f / 2.2f));
}

/* whether a reflection's texel is HALO_LT_E1's pass's: the structure's
(the pre-pass's surfaces, whose guides the pass traced from, glossy at
their roughness and denoised). The level's glass and water are not in the
pre-pass, nor every model (characters: their pixels' guides can be the
surfaces behind them): a model traces its own mirror ray as
display.rt_reflections does (lt_reflection_inline; its roughness map past
RT_REFLECTION_ROUGHEST keeps its cube map) and takes the lights' shine
(lt_ggx) on top; the glass and water keep their cube maps */
static BOOL lt_reflection_traced(const struct pixel_state *state)
{
	return state->rt_reflection && state->program->rt_reflection == XGPU_RT_REFLECTION_STRUCTURE;
}

/* whether a reflection's texel is display.rt_reflections' own mirror ray
(rt_reflection_texel): without the pass, every tagged draw's; with it, a
model's alone: not the structure's (the pass's), nor the level's glass and
water (transparent passes laid over the structure, b30's shore and a10's
floors among them), whose cube maps keep the dynamic lights' glints the
game draws over them (a hit's light is the lightmap's alone: the plasma's
glow on b30's shore went with it) */
static BOOL lt_reflection_inline(const struct pixel_state *state)
{
	if (!state->rt_reflection)
		return FALSE;
	if (!lt_e1_on())
		return TRUE;
	return state->program->rt_reflection == XGPU_RT_REFLECTION_MODEL;
}

/* HALO_LT_E1's reflections (~/dev/rt2-realtime-design.md section 1.4;
port/linux/vulkan/xgpu_vulkan_lt_indirect.c): the reflection's texel
(texture 3's cube map lookup, whose colour the game's combiners then mask,
tint and fresnel as they did) from the pass's traced and denoised specular,
the right half of display.rt_bounce's binding: its radiance s.rgb (linear,
the hits' share of the lobe's) and the hits' share s.a; where the rays left
the scene (the sky, past their reach), the cube map, as GLSL:
ivec2 size = textureSize(rt_bounce, 0); int half = size.x >> 1;
ivec2 at = ivec2(clamp(int(gl_FragCoord.x), 0, max(half - 1, 0)) + half, min(int(gl_FragCoord.y), size.y - 1));
vec4 s = texelFetch(rt_bounce, at, 0);
texel = vec4(pow(max(s.rgb + (1.0 - s.a) * pow(max(cube.rgb, 0.0), vec3(2.2)), 0.0), vec3(1.0 / 2.2)), cube.a);
(the binding's stand-in, 1x1 of nothing: the cube map) */
static unsigned int lt_reflection_texel(struct module *m, struct pixel_state *state, unsigned int cube)
{
	unsigned int sampled, image, size, half, pixel, x, y, at, s, operands[4], linear, rgb;

	if (!state->rt_bounce)
		rt_bounce_declare(m, state);
	if (!state->frag_coord)
	{
		state->frag_coord = variable(m, m->t_v4, STORAGE_INPUT, "gl_FragCoord");
		decorate1(m, state->frag_coord, DECORATION_BUILT_IN, BUILT_IN_FRAG_COORD);
	}
	sampled = load(m, type_sampled_image(m, DIM_2D, FALSE, FALSE, TRUE), state->rt_bounce);
	image = op(m, OP_IMAGE, type_sampled_image(m, DIM_2D, FALSE, FALSE, FALSE), 1, &sampled);
	size = image_size(m, image, m->t_iv2);
	half = op2(m, OP_SHIFT_RIGHT_ARITHMETIC, m->t_int, op2(m, OP_COMPOSITE_EXTRACT, m->t_int, size, 0), constant_int(m, 1));
	pixel = op1(m, OP_CONVERT_F_TO_S, m->t_iv2, xy(m, load(m, m->t_v4, state->frag_coord)));
	x = op2(m, OP_I_ADD, m->t_int, ext3(m, GLSL_S_CLAMP, m->t_int, op2(m, OP_COMPOSITE_EXTRACT, m->t_int, pixel, 0), constant_int(m, 0),
		ext2(m, GLSL_S_MAX, m->t_int, int_add(m, half, -1), constant_int(m, 0))), half);
	y = ext2(m, GLSL_S_MIN, m->t_int, op2(m, OP_COMPOSITE_EXTRACT, m->t_int, pixel, 1), int_add(m, op2(m, OP_COMPOSITE_EXTRACT, m->t_int, size, 1), -1));
	at = rt_bounce_iv2(m, x, y);
	operands[0] = image;
	operands[1] = at;
	operands[2] = IMAGE_OPERANDS_LOD;
	operands[3] = constant_int(m, 0);
	s = op(m, OP_IMAGE_FETCH, m->t_v4, 4, operands);
	linear = ext2(m, GLSL_POW, m->t_v3, fmax_scalar(m, 3, xyz(m, cube), 0.0f), constant_splat(m, 3, 2.2f));
	/* (the traced light by the cube map's hue: lt_reflection_chroma) */
	rgb = fadd(m, 3, fmul(m, 3, xyz(m, s), ext2(m, GLSL_POW, m->t_v3, lt_reflection_chroma(m, state),
		constant_splat(m, 3, 2.2f))), fscale(m, 3, linear, fsub(m, 1, constant_float(m, 1.0f), extract(m, s, 3))));
	rgb = ext2(m, GLSL_POW, m->t_v3, fmax_scalar(m, 3, rgb, 0.0f), constant_splat(m, 3, 1.0f / 2.2f));
	/* (HALO_RT_REFLECTIONS_VIEW=1: green by the hits' share, red the rest) */
	if (rt_reflections_view() == 1)
		rgb = construct(m, 3, (unsigned int[]){ fsub(m, 1, constant_float(m, 1.0f), extract(m, s, 3)), extract(m, s, 3),
			constant_float(m, 0.0f) });
	return vec4_of(m, rgb, extract(m, cube, 3));
}

/* ---------- the enhanced dynamic lights' glows (xgpu_spirv.h) */

/* With the VR mode's effects: each glow as the light its bolt scatters
toward the eye (xgpu_spirv.h). The vertex shader: the quad that covers the
haze from the head, as GLSL:

vec3 middle = halo.xyz; float size = halo.w;
vec3 to_head = head.xyz - middle; float away = length(to_head); vec3 n = to_head / max(away, 1e-6);
vec3 across = cross(up, n); across = length(across) > 0.1 ? normalize(across) : normalize(cross(right, n));
bool outside = away > 1.5 * size;
vec3 centre = outside ? middle : head.xyz + forward * head.w;
vec3 x = outside ? across : right, y = outside ? cross(n, across) : up;
float half = outside ? size * away / sqrt(away * away - size * size) * 1.1 + 0.03 : head.w * 8.0;
world = centre + (x * corner.x + y * corner.y) * half;
gl_Position = clip[view] * vec4(world, 1.0);

(the haze's outline seen from the head's centre, a tenth larger, and 0.03
world units, 9 cm, for the eyes' offsets from it; with the head within 1.5
sizes of the bolt, a quad across the view, ahead of the head at the near
distance) */
static BOOL glow_vertex_shader(const struct xgpu_spirv_options *options, struct xgpu_spirv_module *module)
{
	struct module m;
	struct words locals = { 0 };
	unsigned int main_id, middle_in, color_in, corner_in, position_out, world_out, color_out, middle_out;
	unsigned int halo, corner, middle, size, head, near, right, up, forward, to_head, away, n, across, across_length;
	unsigned int outside, centre, x, y, half, world, view, first_row, rows[4], world4;
	int row;

	module_begin(&m, EXECUTION_MODEL_VERTEX);
	declare_constants(&m);
	declare_views(&m, XGPU_MULTIVIEW_EYES, options->foveated, FALSE);
	middle_in = interface_variable(&m, m.t_v4, STORAGE_INPUT, XGPU_SPIRV_HALO_MIDDLE, "middle");
	color_in = interface_variable(&m, m.t_v4, STORAGE_INPUT, XGPU_SPIRV_HALO_COLOR, "halo_color");
	corner_in = interface_variable(&m, m.t_v2, STORAGE_INPUT, XGPU_SPIRV_HALO_CORNER, "halo_corner");
	position_out = variable(&m, m.t_v4, STORAGE_OUTPUT, "gl_Position");
	decorate1(&m, position_out, DECORATION_BUILT_IN, BUILT_IN_POSITION);
	world_out = interface_variable(&m, m.t_v3, STORAGE_OUTPUT, 0, "world");
	color_out = interface_variable(&m, m.t_v3, STORAGE_OUTPUT, 1, "color");
	middle_out = interface_variable(&m, m.t_v4, STORAGE_OUTPUT, 2, "halo");

	main_id = function_begin(&m);
	halo = load(&m, m.t_v4, middle_in);
	corner = load(&m, m.t_v2, corner_in);
	middle = xyz(&m, halo);
	size = extract(&m, halo, 3);
	head = vertex_constant_at(&m, XGPU_SPIRV_GLOW_HEAD);
	near = extract(&m, head, 3);
	right = xyz(&m, vertex_constant_at(&m, XGPU_SPIRV_GLOW_AXES));
	up = xyz(&m, vertex_constant_at(&m, XGPU_SPIRV_GLOW_AXES + 1));
	forward = xyz(&m, vertex_constant_at(&m, XGPU_SPIRV_GLOW_AXES + 2));
	to_head = fsub(&m, 3, xyz(&m, head), middle);
	away = ext1(&m, GLSL_LENGTH, m.t_float, to_head);
	n = fdiv(&m, 3, to_head, splat(&m, 3, fmax_scalar(&m, 1, away, 1.0e-6f)));
	across = ext2(&m, GLSL_CROSS, m.t_v3, up, n);
	across_length = ext1(&m, GLSL_LENGTH, m.t_float, across);
	across = select_n(&m, 3, compare(&m, OP_F_ORD_GREATER_THAN, across_length, constant_float(&m, 0.1f)),
		fdiv(&m, 3, across, splat(&m, 3, fmax_scalar(&m, 1, across_length, 1.0e-6f))),
		ext1(&m, GLSL_NORMALIZE, m.t_v3, ext2(&m, GLSL_CROSS, m.t_v3, right, n)));
	outside = compare(&m, OP_F_ORD_GREATER_THAN, away, fmul(&m, 1, size, constant_float(&m, 1.5f)));
	centre = select_n(&m, 3, outside, middle, fadd(&m, 3, xyz(&m, head), fscale(&m, 3, forward, near)));
	x = select_n(&m, 3, outside, across, right);
	y = select_n(&m, 3, outside, ext2(&m, GLSL_CROSS, m.t_v3, n, across), up);
	half = op3(&m, OP_SELECT, m.t_float, outside,
		fadd(&m, 1, fmul(&m, 1, fdiv(&m, 1, fmul(&m, 1, size, away), ext1(&m, GLSL_SQRT, m.t_float,
		fmax_scalar(&m, 1, fsub(&m, 1, fmul(&m, 1, away, away), fmul(&m, 1, size, size)), 1.0e-6f))),
		constant_float(&m, 1.1f)), constant_float(&m, 0.03f)),
		fmul(&m, 1, near, constant_float(&m, 8.0f)));
	world = fadd(&m, 3, centre, fscale(&m, 3, fadd(&m, 3, fscale(&m, 3, x, extract(&m, corner, 0)),
		fscale(&m, 3, y, extract(&m, corner, 1))), half));
	/* each view's clip transform (the eye's, the inset's: xgpu_vulkan_effects.h) */
	view = view_number(&m, XGPU_MULTIVIEW_EYES, options->foveated);
	first_row = op2(&m, OP_I_ADD, m.t_int, op2(&m, OP_I_MUL, m.t_int, view, constant_int(&m, 4)),
		constant_int(&m, XGPU_SPIRV_GLOW_CLIP));
	world4 = vec4_of(&m, world, constant_float(&m, 1.0f));
	for (row = 0; row < 4; row++)
		rows[row] = fdot(&m, world4, vertex_constant(&m, row ? op2(&m, OP_I_ADD, m.t_int, first_row,
			constant_int(&m, row)) : first_row));
	store(&m, position_out, construct(&m, 4, rows));
	store(&m, world_out, world);
	store(&m, color_out, xyz(&m, load(&m, m.t_v4, color_in)));
	store(&m, middle_out, halo);
	return module_end(&m, EXECUTION_MODEL_VERTEX, main_id, &locals, module);
}

/* erf(x), Abramowitz and Stegun's 7.1.27 (within 5e-4):
sign(x) (1 - 1 / (1 + 0.278393 |x| + 0.230389 x^2 + 0.000972 |x|^3 + 0.078108 x^4)^4) */
static unsigned int error_function(struct module *m, unsigned int x)
{
	unsigned int a = ext1(m, GLSL_F_ABS, m->t_float, x), sum, square;

	sum = fadd(m, 1, fmul(m, 1, a, constant_float(m, 0.078108f)), constant_float(m, 0.000972f));
	sum = fadd(m, 1, fmul(m, 1, sum, a), constant_float(m, 0.230389f));
	sum = fadd(m, 1, fmul(m, 1, sum, a), constant_float(m, 0.278393f));
	sum = fadd(m, 1, fmul(m, 1, sum, a), constant_float(m, 1.0f));
	square = fmul(m, 1, sum, sum);
	square = fmul(m, 1, square, square);
	return fmul(m, 1, ext1(m, GLSL_F_SIGN, m->t_float, x),
		fsub(m, 1, constant_float(m, 1.0f), fdiv(m, 1, constant_float(m, 1.0f), square)));
}

/* the fraction of a Gaussian haze exp(-k s^2) (s from its middle, in its
sizes) along a ray that lies between from and to (the ray's distances
past its nearest approach to the middle, in sizes): 0.5 (erf(sqrt(k) to)
- erf(sqrt(k) from)) */
static unsigned int haze_fraction(struct module *m, float root_k, unsigned int from, unsigned int to)
{
	unsigned int k = constant_float(m, root_k);

	return fmul(m, 1, constant_float(m, 0.5f), fsub(m, 1, error_function(m, fmul(m, 1, to, k)),
		error_function(m, fmul(m, 1, from, k))));
}

/* The pixel shader, as GLSL:

vec3 ray = normalize(world - eye[view]); vec3 to_middle = halo.xyz - eye[view];
float nearest = dot(to_middle, ray);                               (along the ray, to its nearest approach)
float r2 = max(dot(to_middle, to_middle) - nearest * nearest, 0.0) / (halo.w * halo.w);
if (r2 >= 1.0) discard;
(the scene's distance along the ray, as the effects': effect_prelude)
float from = -nearest / halo.w, to = (to_scene - nearest) / halo.w;
float glow = (exp(-3.0 * r2) - 0.0498) * 0.6 * fraction(sqrt(3.0), from, to) +
	exp(-24.0 * r2) * 0.8 * fraction(sqrt(24.0), from, to);
fragment_color = vec4(color * max(glow, 0.0), 0.0);

the card's glow at r2, each of its two terms by the part of its haze the
eye sees along the ray: all of it unhidden (from far before the bolt to far
past it), less where the scene's surface is nearer */
static BOOL glow_pixel_shader(const struct xgpu_spirv_options *options, struct xgpu_spirv_module *module)
{
	struct module m;
	struct words locals = { 0 };
	unsigned int main_id, scene_depth, frag_coord, world_in, color_in, halo_in, color_out, view, eye, ray, halo;
	unsigned int to_middle, nearest, r2, size, frag, first_column, columns[4], base, depth, behind, far, to_scene;
	unsigned int from, to, wide, core, glow, operands[4];
	int column;

	module_begin(&m, EXECUTION_MODEL_FRAGMENT);
	declare_uniforms(&m);
	declare_views(&m, XGPU_MULTIVIEW_EYES, options->foveated, TRUE);
	scene_depth = variable(&m, type_sampled_image(&m, DIM_2D, TRUE, FALSE, TRUE), STORAGE_UNIFORM_CONSTANT, "scene_depth");
	decorate1(&m, scene_depth, DECORATION_DESCRIPTOR_SET, XGPU_SPIRV_SET_TEXTURES);
	decorate1(&m, scene_depth, DECORATION_BINDING, 0);
	frag_coord = variable(&m, m.t_v4, STORAGE_INPUT, "gl_FragCoord");
	decorate1(&m, frag_coord, DECORATION_BUILT_IN, BUILT_IN_FRAG_COORD);
	world_in = interface_variable(&m, m.t_v3, STORAGE_INPUT, 0, "world");
	color_in = interface_variable(&m, m.t_v3, STORAGE_INPUT, 1, "color");
	decorate(&m, color_in, DECORATION_FLAT, 0, NULL);
	halo_in = interface_variable(&m, m.t_v4, STORAGE_INPUT, 2, "halo");
	decorate(&m, halo_in, DECORATION_FLAT, 0, NULL);
	color_out = interface_variable(&m, m.t_v4, STORAGE_OUTPUT, 0, "fragment_color");

	main_id = function_begin(&m);
	view = view_number(&m, XGPU_MULTIVIEW_EYES, options->foveated);
	eye = xyz(&m, uniform_index(&m, U_EFFECT_EYE, view));
	ray = ext1(&m, GLSL_NORMALIZE, m.t_v3, fsub(&m, 3, load(&m, m.t_v3, world_in), eye));
	halo = load(&m, m.t_v4, halo_in);
	size = extract(&m, halo, 3);
	to_middle = fsub(&m, 3, xyz(&m, halo), eye);
	nearest = fdot(&m, to_middle, ray);
	r2 = fdiv(&m, 1, fmax_scalar(&m, 1, fsub(&m, 1, fdot(&m, to_middle, to_middle), fmul(&m, 1, nearest, nearest)), 0.0f),
		fmul(&m, 1, size, size));
	discard_if(&m, compare(&m, OP_F_ORD_GREATER_THAN_EQUAL, r2, constant_float(&m, 1.0f)));
	/* the scene's distance along the ray, from its depth in the view's layer */
	frag = load(&m, m.t_v4, frag_coord);
	first_column = op2(&m, OP_I_MUL, m.t_int, view, constant_int(&m, 4));
	for (column = 0; column < 4; column++)
		columns[column] = uniform_index(&m, U_EFFECT_UNPROJECT, column ? op2(&m, OP_I_ADD, m.t_int, first_column,
			constant_int(&m, column)) : first_column);
	base = fadd(&m, 4, fadd(&m, 4, fscale(&m, 4, columns[0], extract(&m, frag, 0)), fscale(&m, 4, columns[1],
		extract(&m, frag, 1))), columns[3]);
	{
		unsigned int sampled = load(&m, type_sampled_image(&m, DIM_2D, TRUE, FALSE, TRUE), scene_depth);
		unsigned int parts[2];

		parts[0] = op1(&m, OP_CONVERT_F_TO_S, m.t_iv2, xy(&m, frag));
		parts[1] = view;
		operands[0] = op(&m, OP_IMAGE, type_sampled_image(&m, DIM_2D, TRUE, FALSE, FALSE), 1, &sampled);
		operands[1] = op(&m, OP_COMPOSITE_CONSTRUCT, m.t_iv3, 2, parts);
		operands[2] = IMAGE_OPERANDS_LOD;
		operands[3] = constant_int(&m, 0);
		depth = effect_game_depth(&m, options, extract(&m, op(&m, OP_IMAGE_FETCH, m.t_v4, 4, operands), 0));
	}
	behind = fadd(&m, 4, base, fscale(&m, 4, columns[2], depth));
	far = op2(&m, OP_LOGICAL_OR, m.t_bool, compare(&m, OP_F_ORD_GREATER_THAN_EQUAL, depth, constant_float(&m, 1.0f)),
		compare(&m, OP_F_ORD_LESS_THAN_EQUAL, extract(&m, behind, 3), constant_float(&m, 0.0f)));
	to_scene = op3(&m, OP_SELECT, m.t_float, far, constant_float(&m, 1.0e9f),
		ext1(&m, GLSL_LENGTH, m.t_float, fsub(&m, 3, effect_point(&m, behind), eye)));
	/* the hazes' parts along the ray, from the eye to the scene */
	from = fdiv(&m, 1, fneg(&m, 1, nearest), size);
	to = fdiv(&m, 1, fsub(&m, 1, to_scene, nearest), size);
	wide = fmul(&m, 1, fmul(&m, 1, fsub(&m, 1, ext1(&m, GLSL_EXP, m.t_float, fmul(&m, 1, constant_float(&m, -3.0f), r2)),
		constant_float(&m, 0.0498f)), constant_float(&m, 0.6f)), haze_fraction(&m, 1.7320508f, from, to));
	core = fmul(&m, 1, fmul(&m, 1, ext1(&m, GLSL_EXP, m.t_float, fmul(&m, 1, constant_float(&m, -24.0f), r2)),
		constant_float(&m, 0.8f)), haze_fraction(&m, 4.8989795f, from, to));
	glow = fmax_scalar(&m, 1, fadd(&m, 1, wide, core), 0.0f);
	store(&m, color_out, vec4_of(&m, fscale(&m, 3, load(&m, m.t_v3, color_in), glow), constant_float(&m, 0.0f)));
	return module_end(&m, EXECUTION_MODEL_FRAGMENT, main_id, &locals, module);
}

BOOL xgpu_spirv_halo_vertex_shader(const struct xgpu_spirv_options *options, struct xgpu_spirv_module *module)
{
	static const unsigned int xy_zw[4] = { 4, 5, 2, 3 }, zw[2] = { 2, 3 };
	struct module m;
	struct words locals = { 0 };
	unsigned int main_id, middle_in, color_in, corner_in, position_out, corner_out, color_out, depth_out, parameters_out;
	unsigned int middle, corner, world, rows[4], clip, depth, viewport_scale, viewport_offset, scale_parts[3], scale;
	unsigned int offset_parts[3], w, sum, position, color, view = 0;
	int component;

	if (options->effect && options->multiview == XGPU_MULTIVIEW_EYES)
		return glow_vertex_shader(options, module);
	module_begin(&m, EXECUTION_MODEL_VERTEX);
	declare_constants(&m);
	declare_uniforms(&m);
	declare_views(&m, options->multiview, options->foveated, FALSE);
	middle_in = interface_variable(&m, m.t_v4, STORAGE_INPUT, XGPU_SPIRV_HALO_MIDDLE, "middle");
	color_in = interface_variable(&m, m.t_v4, STORAGE_INPUT, XGPU_SPIRV_HALO_COLOR, "halo_color");
	corner_in = interface_variable(&m, m.t_v2, STORAGE_INPUT, XGPU_SPIRV_HALO_CORNER, "halo_corner");
	position_out = variable(&m, m.t_v4, STORAGE_OUTPUT, "gl_Position");
	decorate1(&m, position_out, DECORATION_BUILT_IN, BUILT_IN_POSITION);
	corner_out = interface_variable(&m, m.t_v2, STORAGE_OUTPUT, 0, "corner");
	color_out = interface_variable(&m, m.t_v3, STORAGE_OUTPUT, 1, "color");
	depth_out = interface_variable(&m, m.t_float, STORAGE_OUTPUT, 2, "depth");
	parameters_out = interface_variable(&m, m.t_v4, STORAGE_OUTPUT, 3, "depth_parameters");

	main_id = function_begin(&m);
	middle = load(&m, m.t_v4, middle_in);
	corner = load(&m, m.t_v2, corner_in);
	/* world = middle.xyz + (camera_axes[0].xyz * corner.x + camera_axes[1].xyz * corner.y) * middle.w */
	world = fadd(&m, 3, xyz(&m, middle), fscale(&m, 3, fadd(&m, 3,
		fscale(&m, 3, xyz(&m, vertex_constant_at(&m, XGPU_SPIRV_HALO_CAMERA_AXES)), extract(&m, corner, 0)),
		fscale(&m, 3, xyz(&m, vertex_constant_at(&m, XGPU_SPIRV_HALO_CAMERA_AXES + 1)), extract(&m, corner, 1))),
		extract(&m, middle, 3)));
	for (component = 0; component < 4; component++)
	{
		unsigned int row = vertex_constant_at(&m, XGPU_SPIRV_HALO_PROJECTION + component);

		rows[component] = fadd(&m, 1, fdot(&m, world, xyz(&m, row)), extract(&m, row, 3));
	}
	clip = construct(&m, 4, rows);
	depth = rows[3];
	/* (each eye's correction, as the game's programs have it) */
	if (options->multiview)
	{
		unsigned int eye;

		view = view_number(&m, options->multiview, options->foveated);
		eye = options->foveated ? op2(&m, OP_SHIFT_RIGHT_ARITHMETIC, m.t_int, view, constant_int(&m, 1)) : view;
		clip = eye_position(&m, eye, clip);
	}
	/* gl_Position = vec4((clip.xyz * c[4].xyz + (c[5].xyz + vec3(0.5 + screen_offset, 0.5, 0.0) -
	viewport_offset.xyz) * clip.w) / scale, clip.w) */
	viewport_scale = uniform(&m, U_VIEWPORT_SCALE, -1);
	viewport_offset = uniform(&m, U_VIEWPORT_OFFSET, -1);
	for (component = 0; component < 3; component++)
	{
		unsigned int value = extract(&m, viewport_scale, component);

		scale_parts[component] = op3(&m, OP_SELECT, m.t_float,
			compare(&m, OP_F_UNORD_NOT_EQUAL, value, constant_float(&m, 0.0f)), value, constant_float(&m, 1.0f));
	}
	scale = construct(&m, 3, scale_parts);
	offset_parts[0] = fadd(&m, 1, constant_float(&m, 0.5f), uniform(&m, U_SCREEN_OFFSET, -1));
	offset_parts[1] = constant_float(&m, 0.5f);
	offset_parts[2] = constant_float(&m, 0.0f);
	w = extract(&m, clip, 3);
	sum = fadd(&m, 3, fmul(&m, 3, xyz(&m, clip),
		xyz(&m, vertex_constant_at(&m, XGPU_SPIRV_HALO_PROJECTION + 4))),
		fscale(&m, 3, fsub(&m, 3, fadd(&m, 3, xyz(&m, vertex_constant_at(&m, XGPU_SPIRV_HALO_PROJECTION + 5)),
		construct(&m, 3, offset_parts)), xyz(&m, viewport_offset)), w));
	position = vec4_of(&m, fdiv(&m, 3, sum, scale), w);
	/* Vulkan's rows from the top of the framebuffer, y down (xgpu_spirv.h) */
	position = op3(&m, OP_COMPOSITE_INSERT, m.t_v4, fneg(&m, 1, extract(&m, position, 1)), position, 1);
	if (options->multiview && options->foveated)
	{
		unsigned int correction = uniform_index(&m, U_FOVEA_VIEW, view);
		unsigned int corrected = fadd(&m, 2, fmul(&m, 2, xy(&m, position), xy(&m, correction)),
			fscale(&m, 2, shuffle(&m, 2, correction, correction, zw), extract(&m, position, 3)));

		position = shuffle(&m, 4, position, corrected, xy_zw);
	}
	store(&m, position_out, position);
	store(&m, corner_out, corner);
	color = load(&m, m.t_v4, color_in);
	store(&m, color_out, xyz(&m, color));
	store(&m, depth_out, depth);
	store(&m, parameters_out, vertex_constant_at(&m, XGPU_SPIRV_HALO_DEPTH));
	return module_end(&m, EXECUTION_MODEL_VERTEX, main_id, &locals, module);
}

BOOL xgpu_spirv_halo_pixel_shader(const struct xgpu_spirv_options *options, struct xgpu_spirv_module *module)
{
	BOOL layered = options->multiview != XGPU_MULTIVIEW_NONE;
	struct module m;
	struct words locals = { 0 };
	unsigned int main_id, scene_depth, frag_coord, corner_in, color_in, depth_in, parameters_in, color_out;
	unsigned int corner, r2, parameters, coordinates, operands[4], sampled, scene, away, distance, fade, glow, result;

	if (options->effect && options->multiview == XGPU_MULTIVIEW_EYES)
		return glow_pixel_shader(options, module);
	module_begin(&m, EXECUTION_MODEL_FRAGMENT);
	declare_views(&m, options->multiview, options->foveated, TRUE);
	scene_depth = variable(&m, type_sampled_image(&m, DIM_2D, layered, FALSE, TRUE), STORAGE_UNIFORM_CONSTANT, "scene_depth");
	decorate1(&m, scene_depth, DECORATION_DESCRIPTOR_SET, XGPU_SPIRV_SET_TEXTURES);
	decorate1(&m, scene_depth, DECORATION_BINDING, 0);
	frag_coord = variable(&m, m.t_v4, STORAGE_INPUT, "gl_FragCoord");
	decorate1(&m, frag_coord, DECORATION_BUILT_IN, BUILT_IN_FRAG_COORD);
	corner_in = interface_variable(&m, m.t_v2, STORAGE_INPUT, 0, "corner");
	color_in = interface_variable(&m, m.t_v3, STORAGE_INPUT, 1, "color");
	depth_in = interface_variable(&m, m.t_float, STORAGE_INPUT, 2, "depth");
	parameters_in = interface_variable(&m, m.t_v4, STORAGE_INPUT, 3, "depth_parameters");
	decorate(&m, parameters_in, DECORATION_FLAT, 0, NULL);
	color_out = interface_variable(&m, m.t_v4, STORAGE_OUTPUT, 0, "fragment_color");

	main_id = function_begin(&m);
	/* float r2 = dot(corner, corner); if (r2 >= 1.0) discard; */
	corner = load(&m, m.t_v2, corner_in);
	r2 = fdot(&m, corner, corner);
	discard_if(&m, compare(&m, OP_F_ORD_GREATER_THAN_EQUAL, r2, constant_float(&m, 1.0f)));
	/* float scene = texelFetch(scene_depth, ivec2(gl_FragCoord.xy) [, view], 0).r; */
	coordinates = op1(&m, OP_CONVERT_F_TO_S, m.t_iv2, xy(&m, load(&m, m.t_v4, frag_coord)));
	if (layered)
	{
		operands[0] = coordinates;
		operands[1] = load(&m, m.t_int, m.view_index);
		coordinates = op(&m, OP_COMPOSITE_CONSTRUCT, m.t_iv3, 2, operands);
	}
	sampled = load(&m, type_sampled_image(&m, DIM_2D, layered, FALSE, TRUE), scene_depth);
	operands[0] = op(&m, OP_IMAGE, type_sampled_image(&m, DIM_2D, layered, FALSE, FALSE), 1, &sampled);
	operands[1] = coordinates;
	operands[2] = IMAGE_OPERANDS_LOD;
	operands[3] = constant_int(&m, 0);
	scene = extract(&m, op(&m, OP_IMAGE_FETCH, m.t_v4, 4, operands), 0);
	/* (depth reversed: as the game has it, 1 - it) */
	if (options->reversed_depth)
		scene = fsub(&m, 1, constant_float(&m, 1.0f), scene);
	/* float away = scene - depth_parameters.x;
	float distance = away * depth_parameters.y > 0.0 ? depth_parameters.y / away : 1.0e9;
	float fade = scene >= 1.0 ? 1.0 : clamp((distance - depth) / depth_parameters.z, 0.0, 1.0); */
	parameters = load(&m, m.t_v4, parameters_in);
	away = fsub(&m, 1, scene, extract(&m, parameters, 0));
	distance = op3(&m, OP_SELECT, m.t_float, compare(&m, OP_F_ORD_GREATER_THAN, fmul(&m, 1, away, extract(&m, parameters, 1)),
		constant_float(&m, 0.0f)), fdiv(&m, 1, extract(&m, parameters, 1), away), constant_float(&m, 1.0e9f));
	fade = op3(&m, OP_SELECT, m.t_float, compare(&m, OP_F_ORD_GREATER_THAN_EQUAL, scene, constant_float(&m, 1.0f)),
		constant_float(&m, 1.0f), saturate(&m, 1, fdiv(&m, 1, fsub(&m, 1, distance, load(&m, m.t_float, depth_in)),
		extract(&m, parameters, 2))));
	/* a wide soft glow, and a small hot core:
	float glow = (exp(-3.0 * r2) - 0.0498) * 0.6 + exp(-24.0 * r2) * 0.8; */
	glow = fadd(&m, 1, fmul(&m, 1, fsub(&m, 1, ext1(&m, GLSL_EXP, m.t_float, fmul(&m, 1, constant_float(&m, -3.0f), r2)),
		constant_float(&m, 0.0498f)), constant_float(&m, 0.6f)),
		fmul(&m, 1, ext1(&m, GLSL_EXP, m.t_float, fmul(&m, 1, constant_float(&m, -24.0f), r2)), constant_float(&m, 0.8f)));
	/* fragment_color = vec4(color * (glow * fade * fade), 0.0); */
	result = fscale(&m, 3, load(&m, m.t_v3, color_in), fmul(&m, 1, fmul(&m, 1, glow, fade), fade));
	store(&m, color_out, vec4_of(&m, result, constant_float(&m, 0.0f)));
	return module_end(&m, EXECUTION_MODEL_FRAGMENT, main_id, &locals, module);
}
