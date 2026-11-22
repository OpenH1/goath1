#include "plutosvg.h"

#include <stdint.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int plutosvg_version(void)
{
    return PLUTOSVG_VERSION;
}

const char* plutosvg_version_string(void)
{
    return PLUTOSVG_VERSION_STRING;
}

enum {
    TAG_UNKNOWN = 0,
    TAG_CIRCLE,
    TAG_CLIP_PATH, // TODO
    TAG_DEFS,
    TAG_ELLIPSE,
    TAG_FE_COLOR_MATRIX,
    TAG_FE_COMPONENT_TRANSFER,
    TAG_FE_FUNC_A,
    TAG_FE_FUNC_B,
    TAG_FE_FUNC_G,
    TAG_FE_FUNC_R,
    TAG_FE_GAUSSIAN_BLUR,
    TAG_FE_MERGE,
    TAG_FE_MERGE_NODE,
    TAG_FILTER,
    TAG_G,
    TAG_IMAGE,
    TAG_LINE,
    TAG_LINEAR_GRADIENT,
    TAG_MASK,
    TAG_PATH,
    TAG_POLYGON,
    TAG_POLYLINE,
    TAG_RADIAL_GRADIENT,
    TAG_RECT,
    TAG_STOP,
    TAG_SVG,
    TAG_SYMBOL,
    TAG_USE
};

enum {
    ATTR_UNKNOWN = 0,
    ATTR_AMPLITUDE,
    ATTR_CLIP_PATH,
    ATTR_CLIP_PATH_UNITS,
    ATTR_CLIP_RULE,
    ATTR_COLOR,
    ATTR_CX,
    ATTR_CY,
    ATTR_D,
    ATTR_DISPLAY,
    ATTR_EXPONENT,
    ATTR_FILL,
    ATTR_FILL_OPACITY,
    ATTR_FILL_RULE,
    ATTR_FILTER,
    ATTR_FILTER_UNITS,
    ATTR_FX,
    ATTR_FY,
    ATTR_GRADIENT_TRANSFORM,
    ATTR_GRADIENT_UNITS,
    ATTR_HEIGHT,
    ATTR_HREF,
    ATTR_ID,
    ATTR_IN,
    ATTR_INTERCEPT,
    ATTR_MASK,
    ATTR_MASK_TYPE,
    ATTR_MASK_UNITS,
    ATTR_OFFSET,
    ATTR_OPACITY,
    ATTR_POINTS,
    ATTR_PRESERVE_ASPECT_RATIO,
    ATTR_R,
    ATTR_RX,
    ATTR_RY,
    ATTR_SLOPE,
    ATTR_SPREAD_METHOD,
    ATTR_STD_DEVIATION,
    ATTR_STOP_COLOR,
    ATTR_STOP_OPACITY,
    ATTR_STROKE,
    ATTR_STROKE_DASHARRAY,
    ATTR_STROKE_DASHOFFSET,
    ATTR_STROKE_LINECAP,
    ATTR_STROKE_LINEJOIN,
    ATTR_STROKE_MITERLIMIT,
    ATTR_STROKE_OPACITY,
    ATTR_STROKE_WIDTH,
    ATTR_STYLE,
    ATTR_TABLE_VALUES,
    ATTR_TRANSFORM,
    ATTR_TYPE,
    ATTR_VALUES,
    ATTR_VIEW_BOX,
    ATTR_VISIBILITY,
    ATTR_WIDTH,
    ATTR_X,
    ATTR_X1,
    ATTR_X2,
    ATTR_Y,
    ATTR_Y1,
    ATTR_Y2
};

#define MAX_NAME 19

typedef struct {
    const char* name;
    int id;
} name_entry_t;

static int name_entry_compare(const void* a, const void* b)
{
    const char* name = a;
    const name_entry_t* entry = b;
    return strcmp(name, entry->name);
}

static int lookupid(const char* data, size_t length, const name_entry_t* table, size_t count)
{
    if(length > MAX_NAME)
        return 0;
    char name[MAX_NAME + 1];
    for(int i = 0; i < length; i++)
        name[i] = data[i];
    name[length] = '\0';

    name_entry_t* entry = bsearch(name, table, count / sizeof(name_entry_t), sizeof(name_entry_t), name_entry_compare);
    if(entry == NULL)
        return 0;
    return entry->id;
}

static int elementid(const char* data, size_t length)
{
    static const name_entry_t table[] = {
        {"circle", TAG_CIRCLE},
        {"clipPath", TAG_CLIP_PATH},
        {"defs", TAG_DEFS},
        {"ellipse", TAG_ELLIPSE},
        {"feColorMatrix", TAG_FE_COLOR_MATRIX},
        {"feComponentTransfer", TAG_FE_COMPONENT_TRANSFER},
        {"feFuncA", TAG_FE_FUNC_A},
        {"feFuncB", TAG_FE_FUNC_B},
        {"feFuncG", TAG_FE_FUNC_G},
        {"feFuncR", TAG_FE_FUNC_R},
        {"feGaussianBlur", TAG_FE_GAUSSIAN_BLUR},
        {"feMerge", TAG_FE_MERGE},
        {"feMergeNode", TAG_FE_MERGE_NODE},
        {"filter", TAG_FILTER},
        {"g", TAG_G},
        {"image", TAG_IMAGE},
        {"line", TAG_LINE},
        {"linearGradient", TAG_LINEAR_GRADIENT},
        {"mask", TAG_MASK},
        {"path", TAG_PATH},
        {"polygon", TAG_POLYGON},
        {"polyline", TAG_POLYLINE},
        {"radialGradient", TAG_RADIAL_GRADIENT},
        {"rect", TAG_RECT},
        {"stop", TAG_STOP},
        {"svg", TAG_SVG},
        {"symbol", TAG_SYMBOL},
        {"use", TAG_USE}
    };

    return lookupid(data, length, table, sizeof(table));
}

static int attributeid(const char* data, size_t length)
{
    static const name_entry_t table[] = {
        {"amplitude", ATTR_AMPLITUDE},
        {"clip-path", ATTR_CLIP_PATH},
        {"clip-rule", ATTR_CLIP_RULE},
        {"clipPathUnits", ATTR_CLIP_PATH_UNITS},
        {"color", ATTR_COLOR},
        {"cx", ATTR_CX},
        {"cy", ATTR_CY},
        {"d", ATTR_D},
        {"display", ATTR_DISPLAY},
        {"exponent", ATTR_EXPONENT},
        {"fill", ATTR_FILL},
        {"fill-opacity", ATTR_FILL_OPACITY},
        {"fill-rule", ATTR_FILL_RULE},
        {"filter", ATTR_FILTER},
        {"filterUnits", ATTR_FILTER_UNITS},
        {"fx", ATTR_FX},
        {"fy", ATTR_FY},
        {"gradientTransform", ATTR_GRADIENT_TRANSFORM},
        {"gradientUnits", ATTR_GRADIENT_UNITS},
        {"height", ATTR_HEIGHT},
        {"href", ATTR_HREF},
        {"id", ATTR_ID},
        {"in", ATTR_IN},
        {"intercept", ATTR_INTERCEPT},
        {"mask", ATTR_MASK},
        {"mask-type", ATTR_MASK_TYPE},
        {"maskUnits", ATTR_MASK_UNITS},
        {"offset", ATTR_OFFSET},
        {"opacity", ATTR_OPACITY},
        {"points", ATTR_POINTS},
        {"preserveAspectRatio", ATTR_PRESERVE_ASPECT_RATIO},
        {"r", ATTR_R},
        {"rx", ATTR_RX},
        {"ry", ATTR_RY},
        {"slope", ATTR_SLOPE},
        {"spreadMethod", ATTR_SPREAD_METHOD},
        {"stdDeviation", ATTR_STD_DEVIATION},
        {"stop-color", ATTR_STOP_COLOR},
        {"stop-opacity", ATTR_STOP_OPACITY},
        {"stroke", ATTR_STROKE},
        {"stroke-dasharray", ATTR_STROKE_DASHARRAY},
        {"stroke-dashoffset", ATTR_STROKE_DASHOFFSET},
        {"stroke-linecap", ATTR_STROKE_LINECAP},
        {"stroke-linejoin", ATTR_STROKE_LINEJOIN},
        {"stroke-miterlimit", ATTR_STROKE_MITERLIMIT},
        {"stroke-opacity", ATTR_STROKE_OPACITY},
        {"stroke-width", ATTR_STROKE_WIDTH},
        {"style", ATTR_STYLE},
        {"tableValues", ATTR_TABLE_VALUES},
        {"transform", ATTR_TRANSFORM},
        {"type", ATTR_TYPE},
        {"values", ATTR_VALUES},
        {"viewBox", ATTR_VIEW_BOX},
        {"visibility", ATTR_VISIBILITY},
        {"width", ATTR_WIDTH},
        {"x", ATTR_X},
        {"x1", ATTR_X1},
        {"x2", ATTR_X2},
        {"xlink:href", ATTR_HREF},
        {"y", ATTR_Y},
        {"y1", ATTR_Y1},
        {"y2", ATTR_Y2}
    };

    return lookupid(data, length, table, sizeof(table));
}

static int cssattributeid(const char* data, size_t length)
{
    static const name_entry_t table[] = {
        {"clip-path", ATTR_CLIP_PATH},
        {"clip-rule", ATTR_CLIP_RULE},
        {"color", ATTR_COLOR},
        {"display", ATTR_DISPLAY},
        {"fill", ATTR_FILL},
        {"fill-opacity", ATTR_FILL_OPACITY},
        {"fill-rule", ATTR_FILL_RULE},
        {"filter", ATTR_FILTER},
        {"mask", ATTR_MASK},
        {"mask-type", ATTR_MASK_TYPE},
        {"opacity", ATTR_OPACITY},
        {"stop-color", ATTR_STOP_COLOR},
        {"stop-opacity", ATTR_STOP_OPACITY},
        {"stroke", ATTR_STROKE},
        {"stroke-dasharray", ATTR_STROKE_DASHARRAY},
        {"stroke-dashoffset", ATTR_STROKE_DASHOFFSET},
        {"stroke-linecap", ATTR_STROKE_LINECAP},
        {"stroke-linejoin", ATTR_STROKE_LINEJOIN},
        {"stroke-miterlimit", ATTR_STROKE_MITERLIMIT},
        {"stroke-opacity", ATTR_STROKE_OPACITY},
        {"stroke-width", ATTR_STROKE_WIDTH},
        {"visibility", ATTR_VISIBILITY}
    };

    return lookupid(data, length, table, sizeof(table));
}

typedef struct {
    const char* data;
    size_t length;
} string_t;

typedef struct attribute {
    int id;
    string_t value;
    struct attribute* next;
} attribute_t;

typedef struct element {
    int id;
    struct element* parent;
    struct element* last_child;
    struct element* first_child;
    struct element* next_sibling;
    struct attribute* attributes;
} element_t;

typedef struct heap_chunk {
    struct heap_chunk* next;
} heap_chunk_t;

typedef struct {
    heap_chunk_t* chunk;
    size_t size;
} heap_t;

static heap_t* heap_create(void)
{
    heap_t* heap = malloc(sizeof(heap_t));
    heap->chunk = NULL;
    heap->size = 0;
    return heap;
}

#define CHUNK_SIZE 4096
#define ALIGN_SIZE(size) (((size) + 7ul) & ~7ul)

static void* heap_alloc(heap_t* heap, size_t size)
{
    size = ALIGN_SIZE(size);
    if(heap->chunk == NULL || heap->size + size > CHUNK_SIZE) {
        heap_chunk_t* chunk = malloc(CHUNK_SIZE + sizeof(heap_chunk_t));
        chunk->next = heap->chunk;
        heap->chunk = chunk;
        heap->size = 0;
    }

    void* data = (char*)(heap->chunk) + sizeof(heap_chunk_t) + heap->size;
    heap->size += size;
    return data;
}

static void heap_destroy(heap_t* heap)
{
    while(heap->chunk) {
        heap_chunk_t* chunk = heap->chunk;
        heap->chunk = chunk->next;
        free(chunk);
    }

    free(heap);
}

typedef struct hashmap_entry {
    size_t hash;
    string_t name;
    void* value;
    struct hashmap_entry* next;
} hashmap_entry_t;

typedef struct {
    hashmap_entry_t** buckets;
    size_t size;
    size_t capacity;
} hashmap_t;

static hashmap_t* hashmap_create(void)
{
    hashmap_t* map = malloc(sizeof(hashmap_t));
    map->buckets = calloc(16, sizeof(hashmap_entry_t*));
    map->size = 0;
    map->capacity = 16;
    return map;
}

static size_t hashmap_hash(const char* data, size_t length)
{
    size_t h = length;
    for(size_t i = 0; i < length; i++) {
        h = h * 31 + *data;
        ++data;
    }

    return h;
}

static bool hashmap_eq(const hashmap_entry_t* entry, const char* data, size_t length)
{
    const string_t* name = &entry->name;
    if(name->length != length)
        return false;
    for(size_t i = 0; i < length; i++) {
        if(data[i] != name->data[i]) {
            return false;
        }
    }

    return true;
}

static void hashmap_expand(hashmap_t* map)
{
    if(map->size > (map->capacity * 3 / 4)) {
        size_t newcapacity = map->capacity << 1;
        hashmap_entry_t** newbuckets = calloc(newcapacity, sizeof(hashmap_entry_t*));
        for(size_t i = 0; i < map->capacity; i++) {
            hashmap_entry_t* entry = map->buckets[i];
            while(entry) {
                hashmap_entry_t* next = entry->next;
                size_t index = entry->hash & (newcapacity - 1);
                entry->next = newbuckets[index];
                newbuckets[index] = entry;
                entry = next;
            }
        }

        free(map->buckets);
        map->buckets = newbuckets;
        map->capacity = newcapacity;
    }
}

static void hashmap_put(hashmap_t* map, heap_t* heap, const char* data, size_t length, void* value)
{
    size_t hash = hashmap_hash(data, length);
    size_t index = hash & (map->capacity - 1);

    hashmap_entry_t** p = &map->buckets[index];
    while(true) {
        hashmap_entry_t* current = *p;
        if(current == NULL) {
            hashmap_entry_t* entry = heap_alloc(heap, sizeof(hashmap_entry_t));
            entry->name.data = data;
            entry->name.length = length;
            entry->hash = hash;
            entry->value = value;
            entry->next = NULL;
            *p = entry;
            map->size += 1;
            hashmap_expand(map);
            break;
        }

        if(current->hash == hash && hashmap_eq(current, data, length)) {
            current->value = value;
            break;
        }

        p = &current->next;
    }
}

static void* hashmap_get(const hashmap_t* map, const char* data, size_t length)
{
    size_t hash = hashmap_hash(data, length);
    size_t index = hash & (map->capacity - 1);

    hashmap_entry_t* entry = map->buckets[index];
    while(entry) {
        if(entry->hash == hash && hashmap_eq(entry, data, length))
            return entry->value;
        entry = entry->next;
    }

    return NULL;
}

static void hashmap_destroy(hashmap_t* map)
{
    if(map == NULL)
        return;
    free(map->buckets);
    free(map);
}

static inline const string_t* find_attribute(const element_t* element, int id, bool inherit)
{
    do {
        const attribute_t* attribute = element->attributes;
        while(attribute != NULL) {
            if(attribute->id == id) {
                const string_t* value = &attribute->value;
                if(inherit && value->length == 7 && strncmp(value->data, "inherit", 7) == 0)
                    break;
                return value;
            }

            attribute = attribute->next;
        }

        element = element->parent;
    } while(inherit && element);
    return NULL;
}

static inline bool has_attribute(const element_t* element, int id)
{
    const attribute_t* attribute = element->attributes;
    while(attribute != NULL) {
        if(attribute->id == id)
            return true;
        attribute = attribute->next;
    }

    return false;
}

#define IS_NUM(c) ((c) >= '0' && (c) <= '9')
#define IS_ALPHA(c) (((c) >= 'a' && (c) <= 'z') || ((c) >= 'A' && (c) <= 'Z'))
#define IS_WS(c) ((c) == ' ' || (c) == '\t' || (c) == '\n' || (c) == '\r')

static inline bool parse_float(const char** begin, const char* end, float* number)
{
    const char* it = *begin;
    float integer = 0;
    float fraction = 0;
    float exponent = 0;
    int sign = 1;
    int expsign = 1;

    if(it < end && *it == '+')
        ++it;
    else if(it < end && *it == '-') {
        ++it;
        sign = -1;
    }

    if(it >= end || (*it != '.' && !IS_NUM(*it)))
        return false;
    if(IS_NUM(*it)) {
        do {
            integer = 10.f * integer + (*it++ - '0');
        } while(it < end && IS_NUM(*it));
    }

    if(it < end && *it == '.') {
        ++it;
        if(it >= end || !IS_NUM(*it))
            return false;
        float divisor = 1.f;
        do {
            fraction = 10.f * fraction + (*it++ - '0');
            divisor *= 10.f;
        } while(it < end && IS_NUM(*it));
        fraction /= divisor;
    }

    if(it + 1 < end && (it[0] == 'e' || it[0] == 'E') && (it[1] != 'x' && it[1] != 'm')) {
        ++it;
        if(it < end && *it == '+')
            ++it;
        else if(it < end && *it == '-') {
            ++it;
            expsign = -1;
        }

        if(it >= end || !IS_NUM(*it))
            return false;
        do {
            exponent = 10 * exponent + (*it++ - '0');
        } while(it < end && IS_NUM(*it));
    }

    *begin = it;
    *number = sign * (integer + fraction);
    if(exponent)
        *number *= powf(10.f, expsign * exponent);
    return *number >= -FLT_MAX && *number <= FLT_MAX;
}

static inline bool skip_string(const char** begin, const char* end, const char* data)
{
    const char* it = *begin;
    while(it < end && *data && *it == *data) {
        ++data;
        ++it;
    }

    if(*data == '\0') {
        *begin = it;
        return true;
    }

    return false;
}

static inline const char* string_find(const char* it, const char* end, const char* data)
{
    while(it < end) {
        const char* begin = it;
        if(skip_string(&it, end, data))
            return begin;
        ++it;
    }

    return NULL;
}

static inline bool skip_delim(const char** begin, const char* end, const char delim)
{
    const char* it = *begin;
    if(it < end && *it == delim) {
        *begin = it + 1;
        return true;
    }

    return false;
}

static inline bool skip_ws(const char** begin, const char* end)
{
    const char* it = *begin;
    while(it < end && IS_WS(*it))
        ++it;
    *begin = it;
    return it < end;
}

static inline bool skip_ws_delim(const char** begin, const char* end, char delim)
{
    const char* it = *begin;
    if(it < end && !IS_WS(*it) && *it != delim)
        return false;
    if(skip_ws(&it, end)) {
        if(skip_delim(&it, end, delim)) {
            skip_ws(&it, end);
        }
    }

    *begin = it;
    return it < end;
}

static inline bool skip_ws_comma(const char** begin, const char* end)
{
    return skip_ws_delim(begin, end, ',');
}

static inline const char* rtrim(const char* begin, const char* end)
{
    while(end > begin && IS_WS(end[-1]))
        --end;
    return end;
}

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (hi) < (v) ? (hi) : (v))

static bool parse_number(const element_t* element, int id, float* number, bool percent, bool inherit)
{
    const string_t* value = find_attribute(element, id, inherit);
    if(value == NULL)
        return false;
    const char* it = value->data;
    const char* end = it + value->length;
    if(!parse_float(&it, end, number))
        return false;
    if(percent) {
        if(skip_delim(&it, end, '%'))
            *number /= 100.f;
        *number = CLAMP(*number, 0.f, 1.f);
    }

    return true;
}

typedef enum {
    length_type_unknown,
    length_type_fixed,
    length_type_percent
} length_type_t;

typedef struct {
    float value;
    length_type_t type;
} length_t;

#define is_length_zero(length) ((length).value == 0)
#define is_length_valid(length) ((length).type != length_type_unknown)

static bool parse_length_value(const char** begin, const char* end, length_t* length, bool negative)
{
    float value = 0;
    const char* it = *begin;
    if(!parse_float(&it, end, &value))
        return false;
    if(!negative && value < 0.f) {
        return false;
    }

    char units[2] = {0, 0};
    if(it + 0 < end)
        units[0] = it[0];
    if(it + 1 < end) {
        units[1] = it[1];
    }

    static const float dpi = 96.f;
    switch(units[0]) {
    case '%':
        length->value = value;
        length->type = length_type_percent;
        it += 1;
        break;
    case 'p':
        if(units[1] == 'x')
            length->value = value;
        else if(units[1] == 'c')
            length->value = value * dpi / 6.f;
        else if(units[1] == 't')
            length->value = value * dpi / 72.f;
        else
            return false;
        length->type = length_type_fixed;
        it += 2;
        break;
    case 'i':
        if(units[1] == 'n')
            length->value = value * dpi;
        else
            return false;
        length->type = length_type_fixed;
        it += 2;
        break;
    case 'c':
        if(units[1] == 'm')
            length->value = value * dpi / 2.54f;
        else
            return false;
        length->type = length_type_fixed;
        it += 2;
        break;
    case 'm':
        if(units[1] == 'm')
            length->value = value * dpi / 25.4f;
        else
            return false;
        length->type = length_type_fixed;
        it += 2;
        break;
    default:
        length->value = value;
        length->type = length_type_fixed;
        break;
    }

    *begin = it;
    return true;
}

static bool parse_length(const element_t* element, int id, length_t* length, bool negative, bool inherit)
{
    const string_t* value = find_attribute(element, id, inherit);
    if(value == NULL)
        return false;
    const char* it = value->data;
    const char* end = it + value->length;
    if(parse_length_value(&it, end, length, negative))
        return it == end;
    return false;
}

static inline float convert_length(const length_t* length, float maximum)
{
    if(length->type == length_type_percent)
        return length->value * maximum / 100.f;
    return length->value;
}

typedef enum {
    color_type_fixed,
    color_type_current
} color_type_t;

typedef struct {
    color_type_t type;
    uint32_t value;
} color_t;

typedef enum {
    paint_type_none,
    paint_type_color,
    paint_type_url,
    paint_type_var
} paint_type_t;

typedef struct {
    paint_type_t type;
    color_t color;
    string_t id;
} paint_t;

static bool parse_color_value(const char** begin, const char* end, color_t* color)
{
    const char* it = *begin;
    if(skip_string(&it, end, "currentColor")) {
        color->type = color_type_current;
        color->value = 0xFF000000;
    } else {
        plutovg_color_t value;
        int length = plutovg_color_parse(&value, it, end - it);
        if(length == 0)
            return false;
        color->type = color_type_fixed;
        color->value = plutovg_color_to_argb32(&value);
        it += length;
    }

    *begin = it;
    skip_ws(begin, end);
    return true;
}

static bool parse_color(const element_t* element, int id, color_t* color, bool inherit)
{
    const string_t* value = find_attribute(element, id, inherit);
    if(value == NULL)
        return false;
    const char* it = value->data;
    const char* end = it + value->length;
    if(parse_color_value(&it, end, color))
        return it == end;
    return false;
}

static bool parse_url_value(const char** begin, const char* end, string_t* id)
{
    const char* it = *begin;
    if(!skip_string(&it, end, "url")
        || !skip_ws(&it, end)
        || !skip_delim(&it, end, '(')
        || !skip_ws(&it, end)) {
        return false;
    }

    if(!skip_delim(&it, end, '#'))
        return false;
    id->data = it;
    id->length = 0;
    while(it < end && *it != ')') {
        ++id->length;
        ++it;
    }

    if(!skip_delim(&it, end, ')'))
        return false;
    *begin = it;
    skip_ws(begin, end);
    return true;
}

#define IS_STARTNAMECHAR(c) (IS_ALPHA(c) ||  (c) == '_' || (c) == ':')
#define IS_NAMECHAR(c) (IS_STARTNAMECHAR(c) || IS_NUM(c) || (c) == '-' || (c) == '.')

static bool parse_paint(const element_t* element, int id, paint_t* paint)
{
    const string_t* value = find_attribute(element, id, true);
    if(value == NULL)
        return false;
    const char* it = value->data;
    const char* end = it + value->length;
    if(skip_string(&it, end, "none")) {
        paint->type = paint_type_none;
        return !skip_ws(&it, end);
    }

    if(parse_url_value(&it, end, &paint->id)) {
        paint->type = paint_type_url;
        paint->color.value = 0x00000000;
        if(skip_ws(&it, end)) {
            if(!parse_color_value(&it, end, &paint->color)) {
                return false;
            }
        }

        return it == end;
    }

    if(skip_string(&it, end, "var")) {
        if(!skip_ws(&it, end)
            || !skip_delim(&it, end, '(')
            || !skip_ws(&it, end)) {
            return false;
        }

        if(!skip_string(&it, end, "--"))
            return false;
        const char* begin = it;
        while(it < end && IS_NAMECHAR(*it))
            ++it;
        paint->type = paint_type_var;
        paint->id.data = begin;
        paint->id.length = it - begin;
        paint->color.value = 0x00000000;
        skip_ws(&it, end);
        if(skip_delim(&it, end, ',')) {
            skip_ws(&it, end);
            if(!parse_color_value(&it, end, &paint->color)) {
                return false;
            }
        }

        return skip_delim(&it, end, ')') && !skip_ws(&it, end);
    }

    if(parse_color_value(&it, end, &paint->color)) {
        paint->type = paint_type_color;
        return it == end;
    }

    return false;
}

static bool parse_view_box(const element_t* element, int id, plutovg_rect_t* view_box)
{
    const string_t* value = find_attribute(element, id, false);
    if(value == NULL)
        return false;
    const char* it = value->data;
    const char* end = it + value->length;

    float x, y, w, h;
    if(!parse_float(&it, end, &x)
        || !skip_ws_comma(&it, end)
        || !parse_float(&it, end, &y)
        || !skip_ws_comma(&it, end)
        || !parse_float(&it, end, &w)
        || !skip_ws_comma(&it, end)
        || !parse_float(&it, end, &h)
        || skip_ws(&it, end)) {
        return false;
    }

    if(w <= 0.f || h <= 0.f)
        return false;
    view_box->x = x;
    view_box->y = y;
    view_box->w = w;
    view_box->h = h;
    return true;
}

typedef enum {
    view_align_none,
    view_align_x_min_y_min,
    view_align_x_mid_y_min,
    view_align_x_max_y_min,
    view_align_x_min_y_mid,
    view_align_x_mid_y_mid,
    view_align_x_max_y_mid,
    view_align_x_min_y_max,
    view_align_x_mid_y_max,
    view_align_x_max_y_max
} view_align_t;

typedef enum {
    view_scale_meet,
    view_scale_slice
} view_scale_t;

typedef struct {
    view_align_t align;
    view_scale_t scale;
} view_position_t;

static bool parse_view_position(const element_t* element, int id, view_position_t* position)
{
    const string_t* value = find_attribute(element, id, false);
    if(value == NULL)
        return false;
    const char* it = value->data;
    const char* end = it + value->length;
    if(skip_string(&it, end, "none"))
        position->align = view_align_none;
    else if(skip_string(&it, end, "xMinYMin"))
        position->align = view_align_x_min_y_min;
    else if(skip_string(&it, end, "xMidYMin"))
        position->align = view_align_x_mid_y_min;
    else if(skip_string(&it, end, "xMaxYMin"))
        position->align = view_align_x_max_y_min;
    else if(skip_string(&it, end, "xMinYMid"))
        position->align = view_align_x_min_y_mid;
    else if(skip_string(&it, end, "xMidYMid"))
        position->align = view_align_x_mid_y_mid;
    else if(skip_string(&it, end, "xMaxYMid"))
        position->align = view_align_x_max_y_mid;
    else if(skip_string(&it, end, "xMinYMax"))
        position->align = view_align_x_min_y_max;
    else if(skip_string(&it, end, "xMidYMax"))
        position->align = view_align_x_mid_y_max;
    else if(skip_string(&it, end, "xMaxYMax"))
        position->align = view_align_x_max_y_max;
    else
        return false;
    position->scale = view_scale_meet;
    if(position->align != view_align_none) {
        skip_ws(&it, end);
        if(skip_string(&it, end, "meet"))
            position->scale = view_scale_meet;
        else if(skip_string(&it, end, "slice")) {
            position->scale = view_scale_slice;
        }
    }

    return !skip_ws(&it, end);
}

static bool parse_transform(const element_t* element, int id, plutovg_matrix_t* matrix)
{
    const string_t* value = find_attribute(element, id, false);
    if(value == NULL)
        return false;
    return plutovg_matrix_parse(matrix, value->data, value->length);
}

static bool parse_points(const element_t* element, int id, plutovg_path_t* path)
{
    const string_t* value = find_attribute(element, id, false);
    if(value == NULL)
        return false;
    const char* it = value->data;
    const char* end = it + value->length;

    bool requires_move = true;
    while(it < end) {
        float x, y;
        if(!parse_float(&it, end, &x)
            || !skip_ws_comma(&it, end)
            || !parse_float(&it, end, &y)) {
            return false;
        }

        skip_ws_comma(&it, end);
        if(requires_move)
            plutovg_path_move_to(path, x, y);
        else
            plutovg_path_line_to(path, x, y);
        requires_move = false;
    }

    if(element->id == TAG_POLYGON)
        plutovg_path_close(path);
    return true;
}

static bool parse_path(const element_t* element, int id, plutovg_path_t* path)
{
    const string_t* value = find_attribute(element, id, false);
    if(value == NULL)
        return false;
    return plutovg_path_parse(path, value->data, value->length);
}

#define MAX_DASHES 128

typedef struct {
    length_t data[MAX_DASHES];
    size_t size;
} stroke_dash_array_t;

static bool parse_dash_array(const element_t* element, int id, stroke_dash_array_t* dash_array)
{
    const string_t* value = find_attribute(element, id, true);
    if(value == NULL)
        return false;
    const char* it = value->data;
    const char* end = it + value->length;
    while(it < end && dash_array->size < MAX_DASHES) {
        if(!parse_length_value(&it, end, dash_array->data + dash_array->size, false))
            return false;
        skip_ws_comma(&it, end);
        dash_array->size += 1;
    }

    return true;
}

static bool parse_line_cap(const element_t* element, int id, plutovg_line_cap_t* line_cap)
{
    const string_t* value = find_attribute(element, id, true);
    if(value == NULL)
        return false;
    const char* it = value->data;
    const char* end = it + value->length;
    if(skip_string(&it, end, "butt"))
        *line_cap = PLUTOVG_LINE_CAP_BUTT;
    else if(skip_string(&it, end, "round"))
        *line_cap = PLUTOVG_LINE_CAP_ROUND;
    else if(skip_string(&it, end, "square"))
        *line_cap = PLUTOVG_LINE_CAP_SQUARE;
    return !skip_ws(&it, end);
}

static bool parse_line_join(const element_t* element, int id, plutovg_line_join_t* line_join)
{
    const string_t* value = find_attribute(element, id, true);
    if(value == NULL)
        return false;
    const char* it = value->data;
    const char* end = it + value->length;
    if(skip_string(&it, end, "miter"))
        *line_join = PLUTOVG_LINE_JOIN_MITER;
    else if(skip_string(&it, end, "round"))
        *line_join = PLUTOVG_LINE_JOIN_ROUND;
    else if(skip_string(&it, end, "bevel"))
        *line_join = PLUTOVG_LINE_JOIN_BEVEL;
    return !skip_ws(&it, end);
}

static bool parse_fill_rule(const element_t* element, int id, plutovg_fill_rule_t* fill_rule)
{
    const string_t* value = find_attribute(element, id, true);
    if(value == NULL)
        return false;
    const char* it = value->data;
    const char* end = it + value->length;
    if(skip_string(&it, end, "nonzero"))
        *fill_rule = PLUTOVG_FILL_RULE_NON_ZERO;
    else if(skip_string(&it, end, "evenodd"))
        *fill_rule = PLUTOVG_FILL_RULE_EVEN_ODD;
    return !skip_ws(&it, end);
}

static bool parse_spread_method(const element_t* element, int id, plutovg_spread_method_t* spread_method)
{
    const string_t* value = find_attribute(element, id, false);
    if(value == NULL)
        return false;
    const char* it = value->data;
    const char* end = it + value->length;
    if(skip_string(&it, end, "pad"))
        *spread_method = PLUTOVG_SPREAD_METHOD_PAD;
    else if(skip_string(&it, end, "reflect"))
        *spread_method = PLUTOVG_SPREAD_METHOD_REFLECT;
    else if(skip_string(&it, end, "repeat"))
        *spread_method = PLUTOVG_SPREAD_METHOD_REPEAT;
    return !skip_ws(&it, end);
}

typedef enum {
    display_inline,
    display_none
} display_t;

static bool parse_display(const element_t* element, int id, display_t* display)
{
    const string_t* value = find_attribute(element, id, false);
    if(value == NULL)
        return false;
    const char* it = value->data;
    const char* end = it + value->length;
    if(skip_string(&it, end, "inline"))
        *display = display_inline;
    else if(skip_string(&it, end, "none"))
        *display = display_none;
    return !skip_ws(&it, end);
}

typedef enum {
    visibility_visible,
    visibility_hidden,
    visibility_collapse
} visibility_t;

static bool parse_visibility(const element_t* element, int id, visibility_t* visibility)
{
    const string_t* value = find_attribute(element, id, true);
    if(value == NULL)
        return false;
    const char* it = value->data;
    const char* end = it + value->length;
    if(skip_string(&it, end, "visible"))
        *visibility = visibility_visible;
    else if(skip_string(&it, end, "hidden"))
        *visibility = visibility_hidden;
    else if(skip_string(&it, end, "collapse"))
        *visibility = visibility_collapse;
    return !skip_ws(&it, end);
}

typedef enum {
    units_type_object_bounding_box,
    units_type_user_space_on_use
} units_type_t;

static bool parse_units_type(const element_t* element, int id, units_type_t* units_type)
{
    const string_t* value = find_attribute(element, id, false);
    if(value == NULL)
        return false;
    const char* it = value->data;
    const char* end = it + value->length;
    if(skip_string(&it, end, "objectBoundingBox"))
        *units_type = units_type_object_bounding_box;
    else if(skip_string(&it, end, "userSpaceOnUse"))
        *units_type = units_type_user_space_on_use;
    return !skip_ws(&it, end);
}

struct plutosvg_document {
    heap_t* heap;
    plutovg_path_t* path;
    hashmap_t* id_cache;
    element_t* root_element;
    plutovg_destroy_func_t destroy_func;
    void* closure;
    float width;
    float height;
};

static plutosvg_document_t* plutosvg_document_create(float width, float height, plutovg_destroy_func_t destroy_func, void* closure)
{
    plutosvg_document_t* document = malloc(sizeof(plutosvg_document_t));
    document->heap = heap_create();
    document->path = plutovg_path_create();
    document->id_cache = NULL;
    document->root_element = NULL;
    document->destroy_func = destroy_func;
    document->closure = closure;
    document->width = width;
    document->height = height;
    return document;
}

void plutosvg_document_destroy(plutosvg_document_t* document)
{
    if(document == NULL)
        return;
    plutovg_path_destroy(document->path);
    hashmap_destroy(document->id_cache);
    heap_destroy(document->heap);
    if(document->destroy_func)
        document->destroy_func(document->closure);
    free(document);
}

static void add_attribute(element_t* element, plutosvg_document_t* document, int id, const char* data, size_t length)
{
    attribute_t* attribute = heap_alloc(document->heap, sizeof(attribute_t));
    attribute->id = id;
    attribute->value.data = data;
    attribute->value.length = length;
    attribute->next = element->attributes;
    element->attributes = attribute;
}

#define IS_CSS_STARTNAMECHAR(c) (IS_ALPHA(c) || c == '_')
#define IS_CSS_NAMECHAR(c) (IS_CSS_STARTNAMECHAR(c) || IS_NUM(c) || c == '-')

static void parse_style(const char* data, int length, element_t* element, plutosvg_document_t* document)
{
    const char* it = data;
    const char* end = it + length;
    while(it < end && IS_CSS_STARTNAMECHAR(*it)) {
        data = it++;
        while(it < end && IS_CSS_NAMECHAR(*it))
            ++it;
        int id = cssattributeid(data, it - data);
        skip_ws(&it, end);
        if(it >= end || *it != ':')
            return;
        ++it;
        skip_ws(&it, end);
        data = it;
        while(it < end && *it != ';')
            ++it;
        length = rtrim(data, it) - data;
        if(id && element)
            add_attribute(element, document, id, data, length);
        skip_ws_delim(&it, end, ';');
    }
}

static bool parse_attributes(const char** begin, const char* end, element_t* element, plutosvg_document_t* document)
{
    const char* it = *begin;
    while(it < end && IS_STARTNAMECHAR(*it)) {
        const char* data = it++;
        while(it < end && IS_NAMECHAR(*it))
            ++it;
        int id = attributeid(data, it - data);
        skip_ws(&it, end);
        if(it >= end || *it != '=')
            return false;
        ++it;
        skip_ws(&it, end);
        if(it >= end || (*it != '"' && *it != '\''))
            return false;
        const char quote = *it++;
        skip_ws(&it, end);
        data = it;
        while(it < end && *it != quote)
            ++it;
        if(it >= end || *it != quote)
            return false;
        int length = rtrim(data, it) - data;
        if(id && element) {
            if(id == ATTR_ID) {
                if(document->id_cache == NULL)
                    document->id_cache = hashmap_create();
                hashmap_put(document->id_cache, document->heap, data, length, element);
            } else if(id == ATTR_STYLE) {
                parse_style(data, length, element, document);
            } else {
                add_attribute(element, document, id, data, length);
            }
        }

        ++it;
        skip_ws(&it, end);
    }

    *begin = it;
    return true;
}

plutosvg_document_t* plutosvg_document_load_from_data(const char* data, int length, float width, float height, plutovg_destroy_func_t destroy_func, void* closure)
{
    if(length == -1)
        length = strlen(data);
    if(length >= 3) {
        const uint8_t* buffer = (const uint8_t*)(data);

        const uint8_t c1 = buffer[0];
        const uint8_t c2 = buffer[1];
        const uint8_t c3 = buffer[2];
        if(c1 == 0xEF && c2 == 0xBB && c3 == 0xBF) {
            data += 3;
            length -= 3;
        }
    }

    const char* it = data;
    const char* end = it + length;

    plutosvg_document_t* document = plutosvg_document_create(width, height, destroy_func, closure);
    element_t* current = NULL;
    int ignoring = 0;
    while(it < end) {
        if(current == NULL) {
            while(it < end && IS_WS(*it))
                ++it;
            if(it >= end) {
                break;
            }
        } else {
            while(it < end && *it != '<') {
                ++it;
            }
        }

        if(it >= end || *it != '<')
            goto error;
        ++it;
        if(it < end && *it == '?') {
            ++it;
            if(!skip_string(&it, end, "xml"))
                goto error;
            skip_ws(&it, end);
            if(!parse_attributes(&it, end, NULL, NULL))
                goto error;
            if(!skip_string(&it, end, "?>"))
                goto error;
            skip_ws(&it, end);
            continue;
        }

        if(it < end && *it == '!') {
            ++it;
            if(skip_string(&it, end, "--")) {
                const char* begin = string_find(it, end, "-->");
                if(begin == NULL)
                    goto error;
                it = begin + 3;
                skip_ws(&it, end);
                continue;
            }

            if(skip_string(&it, end, "[CDATA[")) {
                const char* begin = string_find(it, end, "]]>");
                if(begin == NULL)
                    goto error;
                it = begin + 3;
                skip_ws(&it, end);
                continue;
            }

            if(skip_string(&it, end, "DOCTYPE")) {
                while(it < end && *it != '>') {
                    if(*it == '[') {
                        ++it;
                        int depth = 1;
                        while(it < end && depth > 0) {
                            if(*it == '[') ++depth;
                            else if(*it == ']') --depth;
                            ++it;
                        }
                    } else {
                        ++it;
                    }
                }

                if(!skip_delim(&it, end, '>'))
                    goto error;
                skip_ws(&it, end);
                continue;
            }

            goto error;
        }

        if(it < end && *it == '/') {
            if(current == NULL && ignoring == 0)
                goto error;
            ++it;
            if(it >= end || !IS_STARTNAMECHAR(*it))
                goto error;
            const char* begin = it++;
            while(it < end && IS_NAMECHAR(*it))
                ++it;
            if(ignoring == 0) {
                int id = elementid(begin, it - begin);
                if(id != current->id)
                    goto error;
                current = current->parent;
            } else {
                --ignoring;
            }

            skip_ws(&it, end);
            if(it >= end || *it != '>')
                goto error;
            ++it;
            continue;
        }

        if(it >= end || !IS_STARTNAMECHAR(*it))
            goto error;
        const char* begin = it++;
        while(it < end && IS_NAMECHAR(*it))
            ++it;
        element_t* element = NULL;
        if(ignoring > 0) {
            ++ignoring;
        } else {
            int id = elementid(begin, it - begin);
            if(id == TAG_UNKNOWN) {
                ignoring = 1;
            } else {
                if(document->root_element && current == NULL)
                    goto error;
                element = heap_alloc(document->heap, sizeof(element_t));
                element->id = id;
                element->parent = NULL;
                element->next_sibling = NULL;
                element->first_child = NULL;
                element->last_child = NULL;
                element->attributes = NULL;
                if(document->root_element == NULL) {
                    if(element->id != TAG_SVG)
                        goto error;
                    document->root_element = element;
                } else {
                    element->parent = current;
                    if(current->last_child) {
                        current->last_child->next_sibling = element;
                        current->last_child = element;
                    } else {
                        current->last_child = element;
                        current->first_child = element;
                    }
                }
            }
        }

        skip_ws(&it, end);
        if(!parse_attributes(&it, end, element, document))
            goto error;
        if(it < end && *it == '>') {
            if(element)
                current = element;
            ++it;
            continue;
        }

        if(it < end && *it == '/') {
            ++it;
            if(it >= end || *it != '>')
                goto error;
            if(ignoring > 0)
                --ignoring;
            ++it;
            continue;
        }

        goto error;
    }

    if(it == end && ignoring == 0 && current == NULL && document->root_element) {
        length_t w = {100, length_type_percent};
        length_t h = {100, length_type_percent};

        parse_length(document->root_element, ATTR_WIDTH, &w, false, false);
        parse_length(document->root_element, ATTR_HEIGHT, &h, false, false);

        float intrinsic_width = convert_length(&w, width);
        float intrinsic_height = convert_length(&h, height);
        if(intrinsic_width <= 0.f || intrinsic_height <= 0.f) {
            plutovg_rect_t view_box = {0, 0, 0, 0};
            if(parse_view_box(document->root_element, ATTR_VIEW_BOX, &view_box)) {
                float intrinsic_ratio = view_box.w / view_box.h;
                if(intrinsic_width <= 0.f && intrinsic_height > 0.f) {
                    intrinsic_width = intrinsic_height * intrinsic_ratio;
                } else if(intrinsic_width > 0.f && intrinsic_height <= 0.f) {
                    intrinsic_height = intrinsic_width / intrinsic_ratio;
                } else {
                    intrinsic_width = view_box.w;
                    intrinsic_height = view_box.h;
                }
            } else {
                if(intrinsic_width == -1)
                    intrinsic_width = 300;
                if(intrinsic_height == -1) {
                    intrinsic_height = 150;
                }
            }
        }

        if(intrinsic_width <= 0.f || intrinsic_height <= 0.f)
            goto error;
        document->width = intrinsic_width;
        document->height = intrinsic_height;
        return document;
    }

error:
    plutosvg_document_destroy(document);
    return NULL;
}

static bool plutosvg_load_file(const char* filename, char** data, long* length)
{
    FILE* stream = fopen(filename, "rb");
    if(stream == NULL) {
        return false;
    }

    char* content = NULL;
    bool success = false;

    fseek(stream, 0, SEEK_END);
    long size = ftell(stream);
    if(size == -1L) {
        goto cleanup;
    }

    content = malloc(size);
    if(content == NULL) {
        goto cleanup;
    }

    fseek(stream, 0, SEEK_SET);
    if(fread(content, 1, size, stream) == size) {
        *data = content;
        *length = size;

        content = NULL;
        success = true;
    }

cleanup:
    fclose(stream);
    free(content);
    return success;
}

plutosvg_document_t* plutosvg_document_load_from_file(const char* filename, float width, float height)
{
    char* data = NULL;
    long length = 0L;
    if(!plutosvg_load_file(filename, &data, &length))
        return NULL;
    return plutosvg_document_load_from_data(data, length, width, height, free, data);
}

typedef enum render_mode {
    render_mode_painting,
    render_mode_clipping,
    render_mode_bounding
} render_mode_t;

typedef struct render_state {
    struct render_state* parent;
    const element_t* element;
    render_mode_t mode;
    float opacity;

    float view_width;
    float view_height;

    plutovg_matrix_t matrix;
    plutovg_rect_t extents;
    /* (OpenH1) the element drawn on a layer of its own, whose opacity the
    layer is drawn with (render_layered) */
    const struct element* layered;
} render_state_t;

#define INVALID_RECT PLUTOVG_MAKE_RECT(0, 0, -1, -1)
#define EMPTY_RECT PLUTOVG_MAKE_RECT(0, 0, 0, 0)

#define IS_INVALID_RECT(rect) ((rect).w < 0 || (rect).h < 0)
#define IS_EMPTY_RECT(rect) ((rect).w <= 0 || (rect).h <= 0)

static void render_state_begin(const element_t* element, render_state_t* state, render_state_t* parent)
{
    state->parent = parent;
    state->element = element;
    state->mode = parent->mode;
    state->opacity = parent->opacity;
    state->matrix = parent->matrix;
    state->extents = INVALID_RECT;
    state->layered = NULL;

    state->view_width = parent->view_width;
    state->view_height = parent->view_height;

    if(element->parent && parse_transform(element, ATTR_TRANSFORM, &state->matrix))
        plutovg_matrix_multiply(&state->matrix, &state->matrix, &parent->matrix);
    if(state->mode == render_mode_painting && parent->layered != element) {
        if(parse_number(element, ATTR_OPACITY, &state->opacity, true, false)) {
            state->opacity *= parent->opacity;
        }
    }
}

static void render_state_end(render_state_t* state)
{
    if(state->mode == render_mode_painting)
        return;
    if(IS_INVALID_RECT(state->extents)) {
        return;
    }

    plutovg_matrix_t matrix;
    plutovg_matrix_invert(&state->parent->matrix, &matrix);
    plutovg_matrix_multiply(&matrix, &state->matrix, &matrix);

    plutovg_rect_t extents;
    plutovg_matrix_map_rect(&matrix, &state->extents, &extents);
    if(IS_INVALID_RECT(state->parent->extents)) {
        state->parent->extents = extents;
        return;
    }

    float l = MIN(state->parent->extents.x, extents.x);
    float t = MIN(state->parent->extents.y, extents.y);
    float r = MAX(state->parent->extents.x + state->parent->extents.w, extents.x + extents.w);
    float b = MAX(state->parent->extents.y + state->parent->extents.h, extents.y + extents.h);

    state->parent->extents.x = l;
    state->parent->extents.y = t;
    state->parent->extents.w = r - l;
    state->parent->extents.h = b - t;
}

static bool has_cycle_reference(const render_state_t* state, const element_t* element)
{
    do {
        if(element == state->element)
            return true;
        state = state->parent;
    } while(state);
    return false;
}

typedef struct {
    const plutosvg_document_t* document;
    plutovg_canvas_t* canvas;
    const plutovg_color_t* current_color;
    plutosvg_palette_func_t palette_func;
    void* closure;
    int depth;
} render_context_t;

static float resolve_length(const render_state_t* state, const length_t* length, char mode)
{
    float maximum = 0.f;
    if(length->type == length_type_percent) {
        if(mode == 'x') {
            maximum = state->view_width;
        } else if(mode == 'y') {
            maximum = state->view_height;
        } else if(mode == 'o') {
            maximum = hypotf(state->view_width, state->view_height) / PLUTOVG_SQRT2;
        }
    }

    return convert_length(length, maximum);
}

static element_t* find_element(const plutosvg_document_t* document, const string_t* id)
{
    if(document->id_cache && id->length > 0)
        return hashmap_get(document->id_cache, id->data, id->length);
    return NULL;
}

static element_t* resolve_href(const plutosvg_document_t* document, const element_t* element)
{
    const string_t* value = find_attribute(element, ATTR_HREF, false);
    if(value && value->length > 1 && value->data[0] == '#') {
        string_t id = {value->data + 1, value->length - 1};
        return find_element(document, &id);
    }

    return NULL;
}

static plutovg_color_t convert_color(const color_t* color)
{
    plutovg_color_t value;
    plutovg_color_init_argb32(&value, color->value);
    return value;
}

static plutovg_color_t resolve_current_color(render_context_t* context, const element_t* element)
{
    color_t color = {color_type_current};
    parse_color(element, ATTR_COLOR, &color, true);
    if(color.type == color_type_fixed)
        return convert_color(&color);
    if(element->parent == NULL) {
        if(context->current_color)
            return *context->current_color;
        return PLUTOVG_BLACK_COLOR;
    }

    return resolve_current_color(context, element->parent);
}

static plutovg_color_t resolve_color(render_context_t* context, const element_t* element, const color_t* color)
{
    if(color->type == color_type_fixed)
        return convert_color(color);
    return resolve_current_color(context, element);
}

#define MAX_STOPS 64

typedef struct {
    plutovg_gradient_stop_t data[MAX_STOPS];
    size_t size;
} gradient_stop_array_t;

static void resolve_gradient_stops(render_context_t* context, const element_t* element, gradient_stop_array_t* stops)
{
    const element_t* child = element->first_child;
    while(child && stops->size < MAX_STOPS) {
        if(child->id == TAG_STOP) {
            float offset = 0.f;
            float stop_opacity = 1.f;
            color_t stop_color = {color_type_fixed, 0xFF000000};

            parse_number(child, ATTR_OFFSET, &offset, true, false);
            parse_number(child, ATTR_STOP_OPACITY, &stop_opacity, true, false);
            parse_color(child, ATTR_STOP_COLOR, &stop_color, false);

            stops->data[stops->size].offset = offset;
            stops->data[stops->size].color = resolve_color(context, child, &stop_color);
            stops->data[stops->size].color.a *= stop_opacity;
            stops->size += 1;
        }

        child = child->next_sibling;
    }
}

static float resolve_gradient_length(const render_state_t* state, const length_t* length, units_type_t units, char mode)
{
    if(units == units_type_user_space_on_use)
        return resolve_length(state, length, mode);
    return convert_length(length, 1.f);
}

typedef struct {
    const element_t* units;
    const element_t* spread;
    const element_t* transform;
    const element_t* stops;
} gradient_attributes_t;

static void collect_gradient_attributes(const element_t* element, gradient_attributes_t* attributes)
{
    if(attributes->units == NULL && has_attribute(element, ATTR_GRADIENT_UNITS))
        attributes->units = element;
    if(attributes->spread == NULL && has_attribute(element, ATTR_SPREAD_METHOD))
        attributes->spread = element;
    if(attributes->transform == NULL && has_attribute(element, ATTR_GRADIENT_TRANSFORM))
        attributes->transform = element;
    if(attributes->stops == NULL) {
        for(const element_t* child = element->first_child; child; child = child->next_sibling) {
            if(child->id == TAG_STOP) {
                attributes->stops = element;
                break;
            }
        }
    }
}

static void fill_gradient_attributes(const element_t* element, gradient_attributes_t* attributes)
{
    if(attributes->units == NULL) attributes->units = element;
    if(attributes->spread == NULL) attributes->spread = element;
    if(attributes->transform == NULL) attributes->transform = element;
    if(attributes->stops == NULL) {
        attributes->stops = element;
    }
}

static void resolve_gradient_attributes(render_context_t* context, const render_state_t* state, const gradient_attributes_t* attributes, units_type_t* units, plutovg_spread_method_t* spread, plutovg_matrix_t* transform, gradient_stop_array_t* stops)
{
    parse_units_type(attributes->units, ATTR_GRADIENT_UNITS, units);
    parse_spread_method(attributes->spread, ATTR_SPREAD_METHOD, spread);
    parse_transform(attributes->transform, ATTR_GRADIENT_TRANSFORM, transform);
    resolve_gradient_stops(context, attributes->stops, stops);
    if(*units == units_type_object_bounding_box) {
        plutovg_matrix_t matrix;
        plutovg_matrix_init_translate(&matrix, state->extents.x, state->extents.y);
        plutovg_matrix_scale(&matrix, state->extents.w, state->extents.h);
        plutovg_matrix_multiply(transform, transform, &matrix);
    }
}

typedef struct {
    gradient_attributes_t base;
    const element_t* x1;
    const element_t* y1;
    const element_t* x2;
    const element_t* y2;
} linear_gradient_attributes_t;

#define MAX_GRADIENT_DEPTH 128

static bool apply_linear_gradient(render_state_t* state, render_context_t* context, const element_t* element)
{
    linear_gradient_attributes_t attributes = {0};
    const element_t* current = element;
    for(int i = 0; i < MAX_GRADIENT_DEPTH; ++i) {
        collect_gradient_attributes(current, &attributes.base);
        if(current->id == TAG_LINEAR_GRADIENT) {
            if(attributes.x1 == NULL && has_attribute(current, ATTR_X1))
                attributes.x1 = current;
            if(attributes.y1 == NULL && has_attribute(current, ATTR_Y1))
                attributes.y1 = current;
            if(attributes.x2 == NULL && has_attribute(current, ATTR_X2))
                attributes.x2 = current;
            if(attributes.y2 == NULL && has_attribute(current, ATTR_Y2)) {
                attributes.y2 = current;
            }
        }

        const element_t* ref = resolve_href(context->document, current);
        if(ref == NULL || !(ref->id == TAG_LINEAR_GRADIENT || ref->id == TAG_RADIAL_GRADIENT))
            break;
        current = ref;
    }

    if(attributes.base.stops == NULL)
        return false;
    fill_gradient_attributes(element, &attributes.base);
    if(attributes.x1 == NULL) attributes.x1 = element;
    if(attributes.y1 == NULL) attributes.y1 = element;
    if(attributes.x2 == NULL) attributes.x2 = element;
    if(attributes.y2 == NULL) attributes.y2 = element;

    units_type_t units = units_type_object_bounding_box;
    plutovg_spread_method_t spread = PLUTOVG_SPREAD_METHOD_PAD;
    plutovg_matrix_t transform = PLUTOVG_IDENTITY_MATRIX;
    gradient_stop_array_t stops = {0};

    resolve_gradient_attributes(context, state, &attributes.base, &units, &spread, &transform, &stops);

    length_t x1 = {0, length_type_fixed};
    length_t y1 = {0, length_type_fixed};
    length_t x2 = {100, length_type_percent};
    length_t y2 = {0, length_type_fixed};

    parse_length(attributes.x1, ATTR_X1, &x1, true, false);
    parse_length(attributes.y1, ATTR_Y1, &y1, true, false);
    parse_length(attributes.x2, ATTR_X2, &x2, true, false);
    parse_length(attributes.y2, ATTR_Y2, &y2, true, false);

    float _x1 = resolve_gradient_length(state, &x1, units, 'x');
    float _y1 = resolve_gradient_length(state, &y1, units, 'y');
    float _x2 = resolve_gradient_length(state, &x2, units, 'x');
    float _y2 = resolve_gradient_length(state, &y2, units, 'y');

    plutovg_canvas_set_linear_gradient(context->canvas, _x1, _y1, _x2, _y2, spread, stops.data, stops.size, &transform);
    return true;
}

typedef struct {
    gradient_attributes_t base;
    const element_t* cx;
    const element_t* cy;
    const element_t* r;
    const element_t* fx;
    const element_t* fy;
} radial_gradient_attributes_t;

static bool apply_radial_gradient(render_state_t* state, render_context_t* context, const element_t* element)
{
    radial_gradient_attributes_t attributes = {0};
    const element_t* current = element;
    for(int i = 0; i < MAX_GRADIENT_DEPTH; ++i) {
        collect_gradient_attributes(current, &attributes.base);
        if(current->id == TAG_RADIAL_GRADIENT) {
            if(attributes.cx == NULL && has_attribute(current, ATTR_CX))
                attributes.cx = current;
            if(attributes.cy == NULL && has_attribute(current, ATTR_CY))
                attributes.cy = current;
            if(attributes.r == NULL && has_attribute(current, ATTR_R))
                attributes.r = current;
            if(attributes.fx == NULL && has_attribute(current, ATTR_FX))
                attributes.fx = current;
            if(attributes.fy == NULL && has_attribute(current, ATTR_FY)) {
                attributes.fy = current;
            }
        }

        const element_t* ref = resolve_href(context->document, current);
        if(ref == NULL || !(ref->id == TAG_LINEAR_GRADIENT || ref->id == TAG_RADIAL_GRADIENT))
            break;
        current = ref;
    }

    if(attributes.base.stops == NULL)
        return false;
    fill_gradient_attributes(element, &attributes.base);
    if(attributes.cx == NULL) attributes.cx = element;
    if(attributes.cy == NULL) attributes.cy = element;
    if(attributes.r == NULL) attributes.r = element;

    units_type_t units = units_type_object_bounding_box;
    plutovg_spread_method_t spread = PLUTOVG_SPREAD_METHOD_PAD;
    plutovg_matrix_t transform = PLUTOVG_IDENTITY_MATRIX;
    gradient_stop_array_t stops = {0};

    resolve_gradient_attributes(context, state, &attributes.base, &units, &spread, &transform, &stops);

    length_t cx = {50, length_type_percent};
    length_t cy = {50, length_type_percent};
    length_t r = {50, length_type_percent};
    length_t fx = {50, length_type_percent};
    length_t fy = {50, length_type_percent};

    parse_length(attributes.cx, ATTR_CX, &cx, true, false);
    parse_length(attributes.cy, ATTR_CY, &cy, true, false);
    parse_length(attributes.r, ATTR_R, &r, false, false);

    if(attributes.fx) {
        parse_length(attributes.fx, ATTR_FX, &fx, true, false);
    } else {
        parse_length(attributes.cx, ATTR_CX, &fx, true, false);
    }

    if(attributes.fy) {
        parse_length(attributes.fy, ATTR_FY, &fy, true, false);
    } else {
        parse_length(attributes.cy, ATTR_CY, &fy, true, false);
    }

    float _cx = resolve_gradient_length(state, &cx, units, 'x');
    float _cy = resolve_gradient_length(state, &cy, units, 'y');
    float _r = resolve_gradient_length(state, &r, units, 'o');
    float _fx = resolve_gradient_length(state, &fx, units, 'x');
    float _fy = resolve_gradient_length(state, &fy, units, 'y');

    plutovg_canvas_set_radial_gradient(context->canvas, _cx, _cy, _r, _fx, _fy, 0.f, spread, stops.data, stops.size, &transform);
    return true;
}

static bool apply_paint(render_state_t* state, render_context_t* context, const paint_t* paint)
{
    if(paint->type == paint_type_none)
        return false;
    if(paint->type == paint_type_color) {
        plutovg_color_t color = resolve_color(context, state->element, &paint->color);
        plutovg_canvas_set_color(context->canvas, &color);
        return true;
    }

    if(paint->type == paint_type_var) {
        plutovg_color_t color;
        if(context->palette_func == NULL || !context->palette_func(context->closure, paint->id.data, paint->id.length, &color))
            color = resolve_color(context, state->element, &paint->color);
        plutovg_canvas_set_color(context->canvas, &color);
        return true;
    }

    const element_t* ref = find_element(context->document, &paint->id);
    if(ref == NULL) {
        plutovg_color_t color = resolve_color(context, state->element, &paint->color);
        plutovg_canvas_set_color(context->canvas, &color);
        return true;
    }

    if(ref->id == TAG_LINEAR_GRADIENT)
        return apply_linear_gradient(state, context, ref);
    if(ref->id == TAG_RADIAL_GRADIENT)
        return apply_radial_gradient(state, context, ref);
    return false;
}

static void draw_shape(const element_t* element, render_context_t* context, render_state_t* state)
{
    paint_t stroke = {paint_type_none};
    parse_paint(element, ATTR_STROKE, &stroke);

    length_t stroke_width = {1.f, length_type_fixed};
    plutovg_line_cap_t line_cap = PLUTOVG_LINE_CAP_BUTT;
    plutovg_line_join_t line_join = PLUTOVG_LINE_JOIN_MITER;
    float miter_limit = 4.f;

    if(stroke.type > paint_type_none) {
        parse_length(element, ATTR_STROKE_WIDTH, &stroke_width, false, true);
        parse_line_cap(element, ATTR_STROKE_LINECAP, &line_cap);
        parse_line_join(element, ATTR_STROKE_LINEJOIN, &line_join);
        parse_number(element, ATTR_STROKE_MITERLIMIT, &miter_limit, false, true);
    }

    if(state->mode == render_mode_bounding) {
        if(stroke.type == paint_type_none)
            return;
        float line_width = resolve_length(state, &stroke_width, 'o');
        float cap_limit = line_width / 2.f;
        if(line_cap == PLUTOVG_LINE_CAP_SQUARE)
            cap_limit *= PLUTOVG_SQRT2;
        float join_limit = line_width / 2.f;
        if(line_join == PLUTOVG_LINE_JOIN_MITER) {
            join_limit *= miter_limit;
        }

        float delta = MAX(cap_limit, join_limit);
        state->extents.x -= delta;
        state->extents.y -= delta;
        state->extents.w += delta * 2.f;
        state->extents.h += delta * 2.f;
        return;
    }

    /* (OpenH1) a clip path's shapes: their fill opaque, by their clip-rule */
    if(state->mode == render_mode_clipping) {
        plutovg_color_t opaque = {0.f, 0.f, 0.f, 1.f};
        plutovg_fill_rule_t clip_rule = PLUTOVG_FILL_RULE_NON_ZERO;
        parse_fill_rule(element, ATTR_CLIP_RULE, &clip_rule);
        plutovg_canvas_set_color(context->canvas, &opaque);
        plutovg_canvas_set_fill_rule(context->canvas, clip_rule);
        plutovg_canvas_set_opacity(context->canvas, 1.f);
        plutovg_canvas_set_matrix(context->canvas, &state->matrix);
        plutovg_canvas_fill_path(context->canvas, context->document->path);
        return;
    }

    paint_t fill = {paint_type_color, {color_type_fixed, 0xFF000000}};
    parse_paint(element, ATTR_FILL, &fill);

    if(apply_paint(state, context, &fill)) {
        float fill_opacity = 1.f;
        parse_number(element, ATTR_FILL_OPACITY, &fill_opacity, true, true);

        plutovg_fill_rule_t fill_rule = PLUTOVG_FILL_RULE_NON_ZERO;
        parse_fill_rule(element, ATTR_FILL_RULE, &fill_rule);

        plutovg_canvas_set_fill_rule(context->canvas, fill_rule);
        plutovg_canvas_set_opacity(context->canvas, fill_opacity * state->opacity);
        plutovg_canvas_set_matrix(context->canvas, &state->matrix);
        plutovg_canvas_fill_path(context->canvas, context->document->path);
    }

    if(apply_paint(state, context, &stroke)) {
        float stroke_opacity = 1.f;
        parse_number(element, ATTR_STROKE_OPACITY, &stroke_opacity, true, true);

        length_t dash_offset = {0.f, length_type_fixed};
        parse_length(element, ATTR_STROKE_DASHOFFSET, &dash_offset, false, true);

        stroke_dash_array_t dash_array = {0};
        parse_dash_array(element, ATTR_STROKE_DASHARRAY, &dash_array);

        float dashes[MAX_DASHES];
        for(int i = 0; i < dash_array.size; ++i) {
            dashes[i] = resolve_length(state, dash_array.data + i, 'o');
        }

        plutovg_canvas_set_dash_offset(context->canvas, resolve_length(state, &dash_offset, 'o'));
        plutovg_canvas_set_dash_array(context->canvas, dashes, dash_array.size);

        plutovg_canvas_set_line_width(context->canvas, resolve_length(state, &stroke_width, 'o'));
        plutovg_canvas_set_line_cap(context->canvas, line_cap);
        plutovg_canvas_set_line_join(context->canvas, line_join);
        plutovg_canvas_set_miter_limit(context->canvas, miter_limit);
        plutovg_canvas_set_opacity(context->canvas, stroke_opacity * state->opacity);
        plutovg_canvas_set_matrix(context->canvas, &state->matrix);
        plutovg_canvas_stroke_path(context->canvas, context->document->path);
    }
}

static bool is_display_none(const element_t* element)
{
    display_t display = display_inline;
    parse_display(element, ATTR_DISPLAY, &display);
    return display == display_none;
}

static bool is_visibility_hidden(const element_t* element)
{
    visibility_t visibility = visibility_visible;
    parse_visibility(element, ATTR_VISIBILITY, &visibility);
    return visibility != visibility_visible;
}

static void render_element(const element_t* element, render_context_t* context, render_state_t* state);
static void render_children(const element_t* element, render_context_t* context, render_state_t* state);

static void apply_view_transform(render_state_t* state, float width, float height)
{
    plutovg_rect_t view_box = {0, 0, 0, 0};
    if(!parse_view_box(state->element, ATTR_VIEW_BOX, &view_box))
        return;
    view_position_t position = {view_align_x_mid_y_mid, view_scale_meet};
    parse_view_position(state->element, ATTR_PRESERVE_ASPECT_RATIO, &position);
    float scale_x = width / view_box.w;
    float scale_y = height / view_box.h;
    if(position.align == view_align_none) {
        plutovg_matrix_scale(&state->matrix, scale_x, scale_y);
        plutovg_matrix_translate(&state->matrix, -view_box.x, -view_box.y);
    } else {
        float scale = (position.scale == view_scale_meet) ? MIN(scale_x, scale_y) : MAX(scale_x, scale_y);
        float offset_x = -view_box.x * scale;
        float offset_y = -view_box.y * scale;
        float view_width = view_box.w * scale;
        float view_height = view_box.h * scale;
        switch(position.align) {
        case view_align_x_mid_y_min:
        case view_align_x_mid_y_mid:
        case view_align_x_mid_y_max:
            offset_x += (width - view_width) * 0.5f;
            break;
        case view_align_x_max_y_min:
        case view_align_x_max_y_mid:
        case view_align_x_max_y_max:
            offset_x += (width - view_width);
            break;
        default:
            break;
        }

        switch(position.align) {
        case view_align_x_min_y_mid:
        case view_align_x_mid_y_mid:
        case view_align_x_max_y_mid:
            offset_y += (height - view_height) * 0.5f;
            break;
        case view_align_x_min_y_max:
        case view_align_x_mid_y_max:
        case view_align_x_max_y_max:
            offset_y += (height - view_height);
            break;
        default:
            break;
        }

        plutovg_matrix_translate(&state->matrix, offset_x, offset_y);
        plutovg_matrix_scale(&state->matrix, scale, scale);
    }

    state->view_width = view_box.w;
    state->view_height = view_box.h;
}

static void render_symbol(const element_t* element, render_context_t* context, render_state_t* state, float x, float y, float width, float height)
{
    if(width <= 0.f || height <= 0.f || is_display_none(element))
        return;
    render_state_t new_state;
    render_state_begin(element, &new_state, state);

    new_state.view_width = width;
    new_state.view_height = height;
    plutovg_matrix_translate(&new_state.matrix, x, y);

    apply_view_transform(&new_state, width, height);
    render_children(element, context, &new_state);
    render_state_end(&new_state);
}

static void render_svg(const element_t* element, render_context_t* context, render_state_t* state)
{
    if(element->parent == NULL) {
        render_symbol(element, context, state, 0.f, 0.f, context->document->width, context->document->height);
        return;
    }

    length_t x = {0, length_type_fixed};
    length_t y = {0, length_type_fixed};

    length_t w = {100, length_type_percent};
    length_t h = {100, length_type_percent};

    parse_length(element, ATTR_X, &x, true, false);
    parse_length(element, ATTR_Y, &y, true, false);
    parse_length(element, ATTR_WIDTH, &w, false, false);
    parse_length(element, ATTR_HEIGHT, &h, false, false);

    float _x = resolve_length(state, &x, 'x');
    float _y = resolve_length(state, &y, 'y');
    float _w = resolve_length(state, &w, 'x');
    float _h = resolve_length(state, &h, 'y');
    render_symbol(element, context, state, _x, _y, _w, _h);
}

static void render_use(const element_t* element, render_context_t* context, render_state_t* state)
{
    if(is_display_none(element) || has_cycle_reference(state, element))
        return;
    element_t* ref = resolve_href(context->document, element);
    if(ref == NULL)
        return;
    length_t x = {0, length_type_fixed};
    length_t y = {0, length_type_fixed};

    parse_length(element, ATTR_X, &x, true, false);
    parse_length(element, ATTR_Y, &y, true, false);

    float _x = resolve_length(state, &x, 'x');
    float _y = resolve_length(state, &y, 'y');

    render_state_t new_state;
    render_state_begin(element, &new_state, state);
    plutovg_matrix_translate(&new_state.matrix, _x, _y);

    const element_t* parent = ref->parent;
    ref->parent = (element_t*)(element);
    if(ref->id == TAG_SYMBOL) {
        /* (OpenH1) a symbol's viewport is the use's width and height */
        length_t w = {100, length_type_percent};
        length_t h = {100, length_type_percent};
        parse_length(element, ATTR_WIDTH, &w, false, false);
        parse_length(element, ATTR_HEIGHT, &h, false, false);
        render_symbol(ref, context, &new_state, 0.f, 0.f, resolve_length(state, &w, 'x'), resolve_length(state, &h, 'y'));
    } else if(ref->id == TAG_SVG) {
        render_svg(ref, context, &new_state);
    } else {
        render_element(ref, context, &new_state);
    }

    ref->parent = (element_t*)(parent);
    render_state_end(&new_state);
}

static void render_g(const element_t* element, render_context_t* context, render_state_t* state)
{
    if(is_display_none(element))
        return;
    render_state_t new_state;
    render_state_begin(element, &new_state, state);
    render_children(element, context, &new_state);
    render_state_end(&new_state);
}

static void render_line(const element_t* element, render_context_t* context, render_state_t* state)
{
    if(is_display_none(element) || is_visibility_hidden(element))
        return;
    length_t x1 = {0, length_type_fixed};
    length_t y1 = {0, length_type_fixed};
    length_t x2 = {0, length_type_fixed};
    length_t y2 = {0, length_type_fixed};

    parse_length(element, ATTR_X1, &x1, true, false);
    parse_length(element, ATTR_Y1, &y1, true, false);
    parse_length(element, ATTR_X2, &x2, true, false);
    parse_length(element, ATTR_Y2, &y2, true, false);

    float _x1 = resolve_length(state, &x1, 'x');
    float _y1 = resolve_length(state, &y1, 'y');
    float _x2 = resolve_length(state, &x2, 'x');
    float _y2 = resolve_length(state, &y2, 'y');

    render_state_t new_state;
    render_state_begin(element, &new_state, state);

    new_state.extents.x = MIN(_x1, _x2);
    new_state.extents.y = MIN(_y1, _y2);
    new_state.extents.w = fabsf(_x2 - _x1);
    new_state.extents.h = fabsf(_y2 - _y1);

    plutovg_path_reset(context->document->path);
    plutovg_path_move_to(context->document->path, _x1, _y1);
    plutovg_path_line_to(context->document->path, _x2, _y2);
    draw_shape(element, context, &new_state);
    render_state_end(&new_state);
}

static void render_ellipse(const element_t* element, render_context_t* context, render_state_t* state)
{
    if(is_display_none(element) || is_visibility_hidden(element))
        return;
    length_t rx = {0, length_type_fixed};
    length_t ry = {0, length_type_fixed};

    parse_length(element, ATTR_RX, &rx, false, false);
    parse_length(element, ATTR_RY, &ry, false, false);
    if(is_length_zero(rx) || is_length_zero(ry))
        return;
    length_t cx = {0, length_type_fixed};
    length_t cy = {0, length_type_fixed};

    parse_length(element, ATTR_CX, &cx, true, false);
    parse_length(element, ATTR_CY, &cy, true, false);

    float _cx = resolve_length(state, &cx, 'x');
    float _cy = resolve_length(state, &cy, 'y');
    float _rx = resolve_length(state, &rx, 'x');
    float _ry = resolve_length(state, &ry, 'y');

    render_state_t new_state;
    render_state_begin(element, &new_state, state);

    new_state.extents.x = _cx - _rx;
    new_state.extents.y = _cy - _ry;
    new_state.extents.w = _rx + _rx;
    new_state.extents.h = _ry + _ry;

    plutovg_path_reset(context->document->path);
    plutovg_path_add_ellipse(context->document->path, _cx, _cy, _rx, _ry);
    draw_shape(element, context, &new_state);
    render_state_end(&new_state);
}

static void render_circle(const element_t* element, render_context_t* context, render_state_t* state)
{
    if(is_display_none(element) || is_visibility_hidden(element))
        return;
    length_t r = {0, length_type_fixed};
    parse_length(element, ATTR_R, &r, false, false);
    if(is_length_zero(r))
        return;
    length_t cx = {0, length_type_fixed};
    length_t cy = {0, length_type_fixed};

    parse_length(element, ATTR_CX, &cx, true, false);
    parse_length(element, ATTR_CY, &cy, true, false);

    float _cx = resolve_length(state, &cx, 'x');
    float _cy = resolve_length(state, &cy, 'y');
    float _r = resolve_length(state, &r, 'o');

    render_state_t new_state;
    render_state_begin(element, &new_state, state);

    new_state.extents.x = _cx - _r;
    new_state.extents.y = _cy - _r;
    new_state.extents.w = _r + _r;
    new_state.extents.h = _r + _r;

    plutovg_path_reset(context->document->path);
    plutovg_path_add_circle(context->document->path, _cx, _cy, _r);
    draw_shape(element, context, &new_state);
    render_state_end(&new_state);
}

static void render_rect(const element_t* element, render_context_t* context, render_state_t* state)
{
    if(is_display_none(element) || is_visibility_hidden(element))
        return;
    length_t w = {0, length_type_fixed};
    length_t h = {0, length_type_fixed};

    parse_length(element, ATTR_WIDTH, &w, false, false);
    parse_length(element, ATTR_HEIGHT, &h, false, false);
    if(is_length_zero(w) || is_length_zero(h))
        return;
    length_t x = {0, length_type_fixed};
    length_t y = {0, length_type_fixed};

    parse_length(element, ATTR_X, &x, true, false);
    parse_length(element, ATTR_Y, &y, true, false);

    float _x = resolve_length(state, &x, 'x');
    float _y = resolve_length(state, &y, 'y');
    float _w = resolve_length(state, &w, 'x');
    float _h = resolve_length(state, &h, 'y');

    length_t rx = {0, length_type_unknown};
    length_t ry = {0, length_type_unknown};

    parse_length(element, ATTR_RX, &rx, false, false);
    parse_length(element, ATTR_RY, &ry, false, false);

    float _rx = resolve_length(state, &rx, 'x');
    float _ry = resolve_length(state, &ry, 'y');

    if(!is_length_valid(rx)) _rx = _ry;
    if(!is_length_valid(ry)) _ry = _rx;

    render_state_t new_state;
    render_state_begin(element, &new_state, state);

    new_state.extents.x = _x;
    new_state.extents.y = _y;
    new_state.extents.w = _w;
    new_state.extents.h = _h;

    plutovg_path_reset(context->document->path);
    plutovg_path_add_round_rect(context->document->path, _x, _y, _w, _h, _rx, _ry);
    draw_shape(element, context, &new_state);
    render_state_end(&new_state);
}

static void render_poly(const element_t* element, render_context_t* context, render_state_t* state)
{
    if(is_display_none(element) || is_visibility_hidden(element))
        return;
    render_state_t new_state;
    render_state_begin(element, &new_state, state);

    plutovg_path_reset(context->document->path);
    parse_points(element, ATTR_POINTS, context->document->path);
    plutovg_path_extents(context->document->path, &new_state.extents, false);
    draw_shape(element, context, &new_state);
    render_state_end(&new_state);
}

static void render_path(const element_t* element, render_context_t* context, render_state_t* state)
{
    if(is_display_none(element) || is_visibility_hidden(element))
        return;
    render_state_t new_state;
    render_state_begin(element, &new_state, state);

    plutovg_path_reset(context->document->path);
    parse_path(element, ATTR_D, context->document->path);
    plutovg_path_extents(context->document->path, &new_state.extents, false);
    draw_shape(element, context, &new_state);
    render_state_end(&new_state);
}

static void transform_view_rect(const view_position_t* position, plutovg_rect_t* dst_rect, plutovg_rect_t* src_rect)
{
    if(position->align == view_align_none)
        return;
    float view_width = dst_rect->w;
    float view_height = dst_rect->h;
    float image_width = src_rect->w;
    float image_height = src_rect->h;
    if(position->scale == view_scale_meet) {
        float scale = image_height / image_width;
        if(view_height > view_width * scale) {
            dst_rect->h = view_width * scale;
            switch(position->align) {
            case view_align_x_min_y_mid:
            case view_align_x_mid_y_mid:
            case view_align_x_max_y_mid:
                dst_rect->y += (view_height - dst_rect->h) * 0.5f;
                break;
            case view_align_x_min_y_max:
            case view_align_x_mid_y_max:
            case view_align_x_max_y_max:
                dst_rect->y += view_height - dst_rect->h;
                break;
            default:
                break;
            }
        }

        if(view_width > view_height / scale) {
            dst_rect->w = view_height / scale;
            switch(position->align) {
            case view_align_x_mid_y_min:
            case view_align_x_mid_y_mid:
            case view_align_x_mid_y_max:
                dst_rect->x += (view_width - dst_rect->w) * 0.5f;
                break;
            case view_align_x_max_y_min:
            case view_align_x_max_y_mid:
            case view_align_x_max_y_max:
                dst_rect->x += view_width - dst_rect->w;
                break;
            default:
                break;
            }
        }
    } else if(position->scale == view_scale_slice) {
        float scale = image_height / image_width;
        if(view_height < view_width * scale) {
            src_rect->h = view_height * (image_width / view_width);
            switch(position->align) {
            case view_align_x_min_y_mid:
            case view_align_x_mid_y_mid:
            case view_align_x_max_y_mid:
                src_rect->y += (image_height - src_rect->h) * 0.5f;
                break;
            case view_align_x_min_y_max:
            case view_align_x_mid_y_max:
            case view_align_x_max_y_max:
                src_rect->y += image_height - src_rect->h;
                break;
            default:
                break;
            }
        }

        if(view_width < view_height / scale) {
            src_rect->w = view_width * (image_height / view_height);
            switch(position->align) {
            case view_align_x_mid_y_min:
            case view_align_x_mid_y_mid:
            case view_align_x_mid_y_max:
                src_rect->x += (image_width - src_rect->w) * 0.5f;
                break;
            case view_align_x_max_y_min:
            case view_align_x_max_y_mid:
            case view_align_x_max_y_max:
                src_rect->x += image_width - src_rect->w;
                break;
            default:
                break;
            }
        }
    }
}

static plutovg_surface_t* load_image(const element_t* element)
{
    const string_t* value = find_attribute(element, ATTR_HREF, false);
    if(value == NULL)
        return NULL;
    const char* it = value->data;
    const char* end = it + value->length;
    if(!skip_string(&it, end, "data:image/png")
        && !skip_string(&it, end, "data:image/jpg")
        && !skip_string(&it, end, "data:image/jpeg")) {
        return NULL;
    }

    if(skip_string(&it, end, ";base64,"))
        return plutovg_surface_load_from_image_base64(it, end - it);
    return NULL;
}

static void draw_image(const element_t* element, render_context_t* context, render_state_t* state, float x, float y, float width, float height)
{
    if(state->mode == render_mode_bounding)
        return;
    plutovg_surface_t* image = load_image(element);
    if(image == NULL)
        return;
    float image_width = plutovg_surface_get_width(image);
    float image_height = plutovg_surface_get_height(image);

    plutovg_rect_t dst_rect = {x, y, width, height};
    plutovg_rect_t src_rect = {0, 0, image_width, image_height};
    view_position_t position = {view_align_x_mid_y_mid, view_scale_meet};

    parse_view_position(element, ATTR_PRESERVE_ASPECT_RATIO, &position);
    transform_view_rect(&position, &dst_rect, &src_rect);

    float scale_x = dst_rect.w / src_rect.w;
    float scale_y = dst_rect.h / src_rect.h;
    plutovg_matrix_t matrix = {scale_x, 0, 0, scale_y, -src_rect.x * scale_x, -src_rect.y * scale_y};

    plutovg_canvas_set_fill_rule(context->canvas, PLUTOVG_FILL_RULE_NON_ZERO);
    plutovg_canvas_set_opacity(context->canvas, state->opacity);
    plutovg_canvas_set_matrix(context->canvas, &state->matrix);
    plutovg_canvas_translate(context->canvas, dst_rect.x, dst_rect.y);
    plutovg_canvas_set_texture(context->canvas, image, PLUTOVG_TEXTURE_TYPE_PLAIN, 1, &matrix);
    plutovg_canvas_fill_rect(context->canvas, 0, 0, dst_rect.w, dst_rect.h);
    plutovg_surface_destroy(image);
}

static void render_image(const element_t* element, render_context_t* context, render_state_t* state)
{
    if(is_display_none(element) || is_visibility_hidden(element))
        return;
    length_t w = {0, length_type_fixed};
    length_t h = {0, length_type_fixed};

    parse_length(element, ATTR_WIDTH, &w, false, false);
    parse_length(element, ATTR_HEIGHT, &h, false, false);
    if(is_length_zero(w) || is_length_zero(h))
        return;
    length_t x = {0, length_type_fixed};
    length_t y = {0, length_type_fixed};

    parse_length(element, ATTR_X, &x, true, false);
    parse_length(element, ATTR_Y, &y, true, false);

    float _x = resolve_length(state, &x, 'x');
    float _y = resolve_length(state, &y, 'y');
    float _w = resolve_length(state, &w, 'x');
    float _h = resolve_length(state, &h, 'y');

    render_state_t new_state;
    render_state_begin(element, &new_state, state);

    new_state.extents.x = _x;
    new_state.extents.y = _y;
    new_state.extents.w = _w;
    new_state.extents.h = _h;
    draw_image(element, context, &new_state, _x, _y, _w, _h);
    render_state_end(&new_state);
}

/* ---------- (OpenH1) layers: clip paths, masks, filters and a group's opacity

An element with a clip-path, a mask or a filter, or a group (g, use, an
inner svg) with an opacity below 1, is drawn on a layer of the canvas's size,
which is then filtered, clipped and masked (in that order, as SVG has it),
and drawn on the canvas with the element's opacity. Only the user space
units these need are read (userSpaceOnUse regions; objectBoundingBox ones
are taken as unbounded), and the filters' colours are worked in sRGB.
The filter primitives drawn: feGaussianBlur, feComponentTransfer (its
feFuncR/G/B/A: identity, table, discrete, linear, gamma), feColorMatrix
(matrix, saturate, luminanceToAlpha) and feMerge, each from SourceGraphic,
SourceAlpha or the previous result; any other primitive passes its input
on. */

typedef struct {
    uint32_t* pixels;
    int width;
    int height;
} layer_image_t;

static const element_t* resolve_url(const render_context_t* context, const element_t* element, int id, int tag)
{
    const string_t* value = find_attribute(element, id, false);
    if(value == NULL)
        return NULL;
    const char* it = value->data;
    const char* end = it + value->length;
    string_t name;
    if(!parse_url_value(&it, end, &name))
        return NULL;
    const element_t* target = find_element(context->document, &name);
    return target && target->id == tag ? target : NULL;
}

static bool attribute_is(const element_t* element, int id, const char* text)
{
    const string_t* value = find_attribute(element, id, false);
    if(value == NULL)
        return false;
    const char* it = value->data;
    const char* end = it + value->length;
    skip_ws(&it, end);
    return skip_string(&it, end, text) && !skip_ws(&it, end);
}

static int parse_number_list(const element_t* element, int id, float* numbers, int maximum)
{
    const string_t* value = find_attribute(element, id, false);
    if(value == NULL)
        return 0;
    const char* it = value->data;
    const char* end = it + value->length;
    int count = 0;
    skip_ws(&it, end);
    while(it < end && count < maximum) {
        if(!parse_float(&it, end, &numbers[count]))
            break;
        ++count;
        skip_ws_comma(&it, end);
    }

    return count;
}

/* a region's rectangle in user space, mapped to the layer's pixels (false:
unbounded) */
static bool layer_region(const element_t* element, int units_id, const plutovg_matrix_t* matrix, int* x0, int* y0, int* x1, int* y1)
{
    units_type_t units = units_type_object_bounding_box;
    parse_units_type(element, units_id, &units);
    float x, y, w, h;
    if(units != units_type_user_space_on_use
        || !parse_number(element, ATTR_X, &x, false, false) || !parse_number(element, ATTR_Y, &y, false, false)
        || !parse_number(element, ATTR_WIDTH, &w, false, false) || !parse_number(element, ATTR_HEIGHT, &h, false, false)) {
        return false;
    }

    plutovg_rect_t rect = {x, y, w, h};
    plutovg_rect_t mapped;
    plutovg_matrix_map_rect(matrix, &rect, &mapped);
    *x0 = (int)floorf(mapped.x);
    *y0 = (int)floorf(mapped.y);
    *x1 = (int)ceilf(mapped.x + mapped.w);
    *y1 = (int)ceilf(mapped.y + mapped.h);
    return true;
}

static void layer_cut_outside(layer_image_t* image, int x0, int y0, int x1, int y1)
{
    for(int y = 0; y < image->height; y++) {
        uint32_t* row = image->pixels + (size_t)y * image->width;
        if(y < y0 || y >= y1) {
            memset(row, 0, (size_t)image->width * 4);
            continue;
        }

        for(int x = 0; x < image->width && x < x0; x++)
            row[x] = 0;
        for(int x = x1 < 0 ? 0 : x1; x < image->width; x++) {
            row[x] = 0;
        }
    }
}

static plutovg_surface_t* layer_surface(const render_context_t* context)
{
    const plutovg_surface_t* target = plutovg_canvas_get_surface(context->canvas);
    return plutovg_surface_create(plutovg_surface_get_width(target), plutovg_surface_get_height(target));
}

static layer_image_t layer_image(plutovg_surface_t* surface)
{
    /* (plutovg's surfaces: rows of 4-byte premultiplied ARGB words, with no padding) */
    layer_image_t image = {(uint32_t*)plutovg_surface_get_data(surface), plutovg_surface_get_width(surface), plutovg_surface_get_height(surface)};
    return image;
}

static uint32_t* image_new(const layer_image_t* like)
{
    return calloc((size_t)like->width * like->height, 4);
}

/* source over destination, the source scaled by opacity (0 to 256) */
static void blend_over(uint32_t* destination, const uint32_t* source, size_t count, uint32_t opacity)
{
    for(size_t index = 0; index < count; index++) {
        uint32_t s = source[index];
        if(s == 0)
            continue;
        if(opacity != 256) {
            s = ((((s >> 8) & 0x00FF00FF) * opacity) & 0xFF00FF00) | ((((s & 0x00FF00FF) * opacity) >> 8) & 0x00FF00FF);
        }

        uint32_t inverse = 255 - (s >> 24);
        uint32_t d = destination[index];
        uint32_t a = (s >> 24) + ((d >> 24) * inverse + 127) / 255;
        uint32_t r = ((s >> 16) & 0xFF) + (((d >> 16) & 0xFF) * inverse + 127) / 255;
        uint32_t g = ((s >> 8) & 0xFF) + (((d >> 8) & 0xFF) * inverse + 127) / 255;
        uint32_t b = (s & 0xFF) + ((d & 0xFF) * inverse + 127) / 255;
        destination[index] = (MIN(a, 255) << 24) | (MIN(r, 255) << 16) | (MIN(g, 255) << 8) | MIN(b, 255);
    }
}

/* each pixel scaled by a coverage of 0 to 255 */
static inline uint32_t pixel_scale(uint32_t pixel, uint32_t coverage)
{
    if(coverage == 255)
        return pixel;
    uint32_t a = (pixel >> 24) * coverage / 255;
    uint32_t r = ((pixel >> 16) & 0xFF) * coverage / 255;
    uint32_t g = ((pixel >> 8) & 0xFF) * coverage / 255;
    uint32_t b = (pixel & 0xFF) * coverage / 255;
    return a << 24 | r << 16 | g << 8 | b;
}

/* ---------- the filters */

/* one box blur of radius r along rows (step 1) or columns (step width), from
source into destination, transparent beyond the edges */
static void box_blur(const uint32_t* source, uint32_t* destination, int count, int lines, int step, int line_step, int radius)
{
    uint32_t size = (uint32_t)(radius * 2 + 1);
    uint32_t reciprocal = (1u << 24) / size;
    for(int line = 0; line < lines; line++) {
        const uint32_t* in = source + (size_t)line * line_step;
        uint32_t* out = destination + (size_t)line * line_step;
        uint32_t sa = 0, sr = 0, sg = 0, sb = 0;
        for(int index = 0; index < radius && index < count; index++) {
            uint32_t p = in[(size_t)index * step];
            sa += p >> 24; sr += (p >> 16) & 0xFF; sg += (p >> 8) & 0xFF; sb += p & 0xFF;
        }

        for(int index = 0; index < count; index++) {
            int entering = index + radius;
            if(entering < count) {
                uint32_t p = in[(size_t)entering * step];
                sa += p >> 24; sr += (p >> 16) & 0xFF; sg += (p >> 8) & 0xFF; sb += p & 0xFF;
            }

            out[(size_t)index * step] = (uint32_t)(((uint64_t)sa * reciprocal) >> 24) << 24
                | (uint32_t)(((uint64_t)sr * reciprocal) >> 24) << 16
                | (uint32_t)(((uint64_t)sg * reciprocal) >> 24) << 8
                | (uint32_t)(((uint64_t)sb * reciprocal) >> 24);
            int leaving = index - radius;
            if(leaving >= 0) {
                uint32_t p = in[(size_t)leaving * step];
                sa -= p >> 24; sr -= (p >> 16) & 0xFF; sg -= (p >> 8) & 0xFF; sb -= p & 0xFF;
            }
        }
    }
}

/* the Gaussian blur as SVG approximates it: three box blurs each way, of
d = floor(s * 3 * sqrt(2 * pi) / 4 + 0.5) */
static void gaussian_blur(uint32_t* pixels, uint32_t* scratch, int width, int height, float deviation_x, float deviation_y)
{
    int dx = (int)floorf(deviation_x * 1.8799712f + 0.5f);
    int dy = (int)floorf(deviation_y * 1.8799712f + 0.5f);
    if(dx > 1) {
        for(int pass = 0; pass < 3; pass++) {
            int radius = (pass == 2 && dx % 2 == 0) ? dx / 2 : (dx - 1) / 2 + (dx % 2 == 0 && pass == 1);
            box_blur(pixels, scratch, width, height, 1, width, radius);
            memcpy(pixels, scratch, (size_t)width * height * 4);
        }
    }

    if(dy > 1) {
        for(int pass = 0; pass < 3; pass++) {
            int radius = (pass == 2 && dy % 2 == 0) ? dy / 2 : (dy - 1) / 2 + (dy % 2 == 0 && pass == 1);
            box_blur(pixels, scratch, height, width, width, 1, radius);
            memcpy(pixels, scratch, (size_t)width * height * 4);
        }
    }
}

/* a transfer function's table of the 256 values of an unpremultiplied
channel (feFuncR/G/B/A) */
static void transfer_table(const element_t* function, uint8_t table[256])
{
    float values[256];
    int count = 0;
    float slope = 1.f, intercept = 0.f, amplitude = 1.f, exponent = 1.f, offset = 0.f;
    int type = 0;
    if(attribute_is(function, ATTR_TYPE, "table")) {
        type = 1;
    } else if(attribute_is(function, ATTR_TYPE, "discrete")) {
        type = 2;
    } else if(attribute_is(function, ATTR_TYPE, "linear")) {
        type = 3;
    } else if(attribute_is(function, ATTR_TYPE, "gamma")) {
        type = 4;
    }

    if(type == 1 || type == 2)
        count = parse_number_list(function, ATTR_TABLE_VALUES, values, 256);
    parse_number(function, ATTR_SLOPE, &slope, false, false);
    parse_number(function, ATTR_INTERCEPT, &intercept, false, false);
    parse_number(function, ATTR_AMPLITUDE, &amplitude, false, false);
    parse_number(function, ATTR_EXPONENT, &exponent, false, false);
    parse_number(function, ATTR_OFFSET, &offset, false, false);
    for(int index = 0; index < 256; index++) {
        float c = index / 255.f, result = c;
        if(type == 1 && count > 0) {
            if(count == 1) {
                result = values[0];
            } else {
                float position = c * (count - 1);
                int k = (int)position;
                if(k >= count - 1) {
                    result = values[count - 1];
                } else {
                    result = values[k] + (position - k) * (values[k + 1] - values[k]);
                }
            }
        } else if(type == 2 && count > 0) {
            int k = (int)(c * count);
            result = values[k >= count ? count - 1 : k];
        } else if(type == 3) {
            result = slope * c + intercept;
        } else if(type == 4) {
            result = amplitude * powf(c, exponent) + offset;
        }

        result = CLAMP(result, 0.f, 1.f);
        table[index] = (uint8_t)(result * 255.f + 0.5f);
    }
}

static void component_transfer(const element_t* primitive, uint32_t* pixels, size_t count)
{
    uint8_t tables[4][256];
    for(int channel = 0; channel < 4; channel++) {
        for(int index = 0; index < 256; index++) {
            tables[channel][index] = (uint8_t)index;
        }
    }

    /* (the tables by channel: 0 alpha, 1 red, 2 green, 3 blue) */
    for(const element_t* child = primitive->first_child; child; child = child->next_sibling) {
        if(child->id == TAG_FE_FUNC_A) {
            transfer_table(child, tables[0]);
        } else if(child->id == TAG_FE_FUNC_R) {
            transfer_table(child, tables[1]);
        } else if(child->id == TAG_FE_FUNC_G) {
            transfer_table(child, tables[2]);
        } else if(child->id == TAG_FE_FUNC_B) {
            transfer_table(child, tables[3]);
        }
    }

    for(size_t index = 0; index < count; index++) {
        uint32_t p = pixels[index];
        uint32_t a = p >> 24, r = 0, g = 0, b = 0;
        if(a) {
            r = MIN(((p >> 16) & 0xFF) * 255 / a, 255);
            g = MIN(((p >> 8) & 0xFF) * 255 / a, 255);
            b = MIN((p & 0xFF) * 255 / a, 255);
        }

        a = tables[0][a];
        r = tables[1][r] * a / 255;
        g = tables[2][g] * a / 255;
        b = tables[3][b] * a / 255;
        pixels[index] = a << 24 | r << 16 | g << 8 | b;
    }
}

static void color_matrix(const element_t* primitive, uint32_t* pixels, size_t count)
{
    float m[20] = {1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0};
    if(attribute_is(primitive, ATTR_TYPE, "saturate")) {
        float s = 1.f;
        parse_number_list(primitive, ATTR_VALUES, &s, 1);
        float saturate[20] = {
            0.213f + 0.787f * s, 0.715f - 0.715f * s, 0.072f - 0.072f * s, 0, 0,
            0.213f - 0.213f * s, 0.715f + 0.285f * s, 0.072f - 0.072f * s, 0, 0,
            0.213f - 0.213f * s, 0.715f - 0.715f * s, 0.072f + 0.928f * s, 0, 0,
            0, 0, 0, 1, 0
        };
        memcpy(m, saturate, sizeof(m));
    } else if(attribute_is(primitive, ATTR_TYPE, "luminanceToAlpha")) {
        float luminance[20] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0.2125f, 0.7154f, 0.0721f, 0, 0};
        memcpy(m, luminance, sizeof(m));
    } else if(!attribute_is(primitive, ATTR_TYPE, "hueRotate")) {
        float values[20];
        if(parse_number_list(primitive, ATTR_VALUES, values, 20) == 20) {
            memcpy(m, values, sizeof(m));
        }
    }

    for(size_t index = 0; index < count; index++) {
        uint32_t p = pixels[index];
        float a = (p >> 24) / 255.f, r = 0, g = 0, b = 0;
        if(p >> 24) {
            r = ((p >> 16) & 0xFF) / 255.f / a;
            g = ((p >> 8) & 0xFF) / 255.f / a;
            b = (p & 0xFF) / 255.f / a;
        }

        float nr = CLAMP(m[0] * r + m[1] * g + m[2] * b + m[3] * a + m[4], 0.f, 1.f);
        float ng = CLAMP(m[5] * r + m[6] * g + m[7] * b + m[8] * a + m[9], 0.f, 1.f);
        float nb = CLAMP(m[10] * r + m[11] * g + m[12] * b + m[13] * a + m[14], 0.f, 1.f);
        float na = CLAMP(m[15] * r + m[16] * g + m[17] * b + m[18] * a + m[19], 0.f, 1.f);
        uint32_t ia = (uint32_t)(na * 255.f + 0.5f);
        pixels[index] = ia << 24 | (uint32_t)(nr * na * 255.f + 0.5f) << 16 | (uint32_t)(ng * na * 255.f + 0.5f) << 8 | (uint32_t)(nb * na * 255.f + 0.5f);
    }
}

#define MAX_FILTER_RESULTS 64

typedef struct {
    const layer_image_t* source;
    uint32_t* source_alpha;
    uint32_t* previous;
    uint32_t* results[MAX_FILTER_RESULTS];
    int result_count;
} filter_inputs_t;

static const uint32_t* filter_input(filter_inputs_t* inputs, const element_t* primitive)
{
    size_t count = (size_t)inputs->source->width * inputs->source->height;
    if(attribute_is(primitive, ATTR_IN, "SourceAlpha")) {
        if(inputs->source_alpha == NULL) {
            inputs->source_alpha = image_new(inputs->source);
            if(inputs->source_alpha == NULL)
                return inputs->source->pixels;
            for(size_t index = 0; index < count; index++) {
                inputs->source_alpha[index] = inputs->source->pixels[index] & 0xFF000000;
            }
        }

        return inputs->source_alpha;
    }

    if(attribute_is(primitive, ATTR_IN, "SourceGraphic") || inputs->previous == NULL)
        return inputs->source->pixels;
    return inputs->previous;
}

static void apply_filter(const element_t* filter, render_context_t* context, layer_image_t* layer, const plutovg_matrix_t* matrix)
{
    size_t count = (size_t)layer->width * layer->height;
    int x0 = 0, y0 = 0, x1 = layer->width, y1 = layer->height;
    bool bounded = layer_region(filter, ATTR_FILTER_UNITS, matrix, &x0, &y0, &x1, &y1);
    float scale_x = sqrtf(matrix->a * matrix->a + matrix->b * matrix->b);
    float scale_y = sqrtf(matrix->c * matrix->c + matrix->d * matrix->d);
    filter_inputs_t inputs = {layer, NULL, NULL, {NULL}, 0};
    uint32_t* scratch = NULL;
    for(const element_t* primitive = filter->first_child; primitive; primitive = primitive->next_sibling) {
        if(inputs.result_count == MAX_FILTER_RESULTS)
            break;
        uint32_t* output = image_new(layer);
        if(output == NULL)
            break;
        inputs.results[inputs.result_count++] = output;
        if(primitive->id == TAG_FE_MERGE) {
            for(const element_t* node = primitive->first_child; node; node = node->next_sibling) {
                if(node->id == TAG_FE_MERGE_NODE) {
                    blend_over(output, filter_input(&inputs, node), count, 256);
                }
            }
        } else {
            memcpy(output, filter_input(&inputs, primitive), count * 4);
            if(primitive->id == TAG_FE_GAUSSIAN_BLUR) {
                float deviation[2] = {0.f, 0.f};
                int numbers = parse_number_list(primitive, ATTR_STD_DEVIATION, deviation, 2);
                if(numbers == 1)
                    deviation[1] = deviation[0];
                if(scratch == NULL)
                    scratch = image_new(layer);
                if(scratch && numbers > 0) {
                    gaussian_blur(output, scratch, layer->width, layer->height, deviation[0] * scale_x, deviation[1] * scale_y);
                }
            } else if(primitive->id == TAG_FE_COMPONENT_TRANSFER) {
                component_transfer(primitive, output, count);
            } else if(primitive->id == TAG_FE_COLOR_MATRIX) {
                color_matrix(primitive, output, count);
            }
        }

        if(bounded) {
            layer_image_t image = {output, layer->width, layer->height};
            layer_cut_outside(&image, x0, y0, x1, y1);
        }

        inputs.previous = output;
    }

    if(inputs.previous)
        memcpy(layer->pixels, inputs.previous, count * 4);
    for(int index = 0; index < inputs.result_count; index++)
        free(inputs.results[index]);
    free(inputs.source_alpha);
    free(scratch);
    (void)context;
}

/* ---------- clip paths and masks */

static void apply_clip(const element_t* clipper, render_context_t* context, render_state_t* state, layer_image_t* layer, const plutovg_matrix_t* matrix)
{
    plutovg_surface_t* surface = layer_surface(context);
    if(surface == NULL)
        return;
    plutovg_canvas_t* canvas = plutovg_canvas_create(surface);
    plutovg_canvas_t* canvas_before = context->canvas;
    render_state_t clip_state = *state;
    clip_state.parent = state;
    clip_state.element = clipper;
    clip_state.mode = render_mode_clipping;
    clip_state.opacity = 1.f;
    clip_state.layered = NULL;
    clip_state.matrix = *matrix;
    plutovg_matrix_t local;
    if(parse_transform(clipper, ATTR_TRANSFORM, &local))
        plutovg_matrix_multiply(&clip_state.matrix, &local, matrix);
    context->canvas = canvas;
    render_children(clipper, context, &clip_state);
    context->canvas = canvas_before;
    layer_image_t clip = layer_image(surface);
    size_t count = (size_t)layer->width * layer->height;
    for(size_t index = 0; index < count; index++)
        layer->pixels[index] = pixel_scale(layer->pixels[index], clip.pixels[index] >> 24);
    plutovg_canvas_destroy(canvas);
    plutovg_surface_destroy(surface);
}

static void apply_mask(const element_t* masker, render_context_t* context, render_state_t* state, layer_image_t* layer, const plutovg_matrix_t* matrix)
{
    plutovg_surface_t* surface = layer_surface(context);
    if(surface == NULL)
        return;
    plutovg_canvas_t* canvas = plutovg_canvas_create(surface);
    plutovg_canvas_t* canvas_before = context->canvas;
    render_state_t mask_state = *state;
    mask_state.parent = state;
    mask_state.element = masker;
    mask_state.mode = render_mode_painting;
    mask_state.opacity = 1.f;
    mask_state.layered = NULL;
    mask_state.matrix = *matrix;
    context->canvas = canvas;
    render_children(masker, context, &mask_state);
    context->canvas = canvas_before;
    layer_image_t mask = layer_image(surface);
    int x0, y0, x1, y1;
    if(layer_region(masker, ATTR_MASK_UNITS, matrix, &x0, &y0, &x1, &y1))
        layer_cut_outside(&mask, x0, y0, x1, y1);
    bool alpha = attribute_is(masker, ATTR_MASK_TYPE, "alpha");
    size_t count = (size_t)layer->width * layer->height;
    for(size_t index = 0; index < count; index++) {
        uint32_t m = mask.pixels[index];
        /* (luminance times alpha, from the premultiplied colour) */
        uint32_t coverage = alpha ? m >> 24 : (54 * ((m >> 16) & 0xFF) + 183 * ((m >> 8) & 0xFF) + 19 * (m & 0xFF)) >> 8;
        layer->pixels[index] = pixel_scale(layer->pixels[index], MIN(coverage, 255));
    }

    plutovg_canvas_destroy(canvas);
    plutovg_surface_destroy(surface);
}

/* whether the element was drawn on a layer of its own (else it is drawn as
it is) */
static bool render_layered(const element_t* element, render_context_t* context, render_state_t* state)
{
    if(state->mode != render_mode_painting || context->canvas == NULL || state->layered == element)
        return false;
    if(is_display_none(element))
        return false;
    const element_t* clipper = resolve_url(context, element, ATTR_CLIP_PATH, TAG_CLIP_PATH);
    const element_t* masker = resolve_url(context, element, ATTR_MASK, TAG_MASK);
    const element_t* filter = resolve_url(context, element, ATTR_FILTER, TAG_FILTER);
    float opacity = 1.f;
    parse_number(element, ATTR_OPACITY, &opacity, true, false);
    bool group = element->id == TAG_G || element->id == TAG_USE || (element->id == TAG_SVG && element->parent);
    if(clipper == NULL && masker == NULL && filter == NULL && (!group || opacity >= 1.f))
        return false;
    if(opacity <= 0.f)
        return true;

    plutovg_surface_t* surface = layer_surface(context);
    if(surface == NULL)
        return true;
    plutovg_canvas_t* canvas = plutovg_canvas_create(surface);
    plutovg_canvas_t* canvas_before = context->canvas;
    render_state_t inner = *state;
    inner.layered = element;
    inner.opacity = 1.f;
    context->canvas = canvas;
    render_element(element, context, &inner);
    context->canvas = canvas_before;

    /* (the element's user space: the regions', clip paths' and masks') */
    plutovg_matrix_t matrix = state->matrix;
    plutovg_matrix_t local;
    if(parse_transform(element, ATTR_TRANSFORM, &local))
        plutovg_matrix_multiply(&matrix, &local, &state->matrix);
    layer_image_t layer = layer_image(surface);
    if(filter)
        apply_filter(filter, context, &layer, &matrix);
    if(clipper)
        apply_clip(clipper, context, state, &layer, &matrix);
    if(masker)
        apply_mask(masker, context, state, &layer, &matrix);

    layer_image_t target = layer_image(plutovg_canvas_get_surface(context->canvas));
    float total = CLAMP(opacity * state->opacity, 0.f, 1.f);
    blend_over(target.pixels, layer.pixels, (size_t)target.width * target.height, (uint32_t)(total * 256.f + 0.5f));
    plutovg_canvas_destroy(canvas);
    plutovg_surface_destroy(surface);
    return true;
}

static bool render_layered(const element_t* element, render_context_t* context, render_state_t* state);

static void render_element(const element_t* element, render_context_t* context, render_state_t* state)
{
    if(render_layered(element, context, state))
        return;
    switch(element->id) {
    case TAG_SVG:
        render_svg(element, context, state);
        break;
    case TAG_USE:
        render_use(element, context, state);
        break;
    case TAG_G:
        render_g(element, context, state);
        break;
    case TAG_LINE:
        render_line(element, context, state);
        break;
    case TAG_ELLIPSE:
        render_ellipse(element, context, state);
        break;
    case TAG_CIRCLE:
        render_circle(element, context, state);
        break;
    case TAG_RECT:
        render_rect(element, context, state);
        break;
    case TAG_POLYLINE:
    case TAG_POLYGON:
        render_poly(element, context, state);
        break;
    case TAG_PATH:
        render_path(element, context, state);
        break;
    case TAG_IMAGE:
        render_image(element, context, state);
        break;
    }
}

#define MAX_RENDER_DEPTH 256

static void render_children(const element_t* element, render_context_t* context, render_state_t* state)
{
    if(context->depth < MAX_RENDER_DEPTH) {
        const element_t* child = element->first_child;
        while(child) {
            context->depth++;
            render_element(child, context, state);
            context->depth--;
            child = child->next_sibling;
        }
    }
}

bool plutosvg_document_render(const plutosvg_document_t* document, const char* id, plutovg_canvas_t* canvas, const plutovg_color_t* current_color, plutosvg_palette_func_t palette_func, void* closure)
{
    render_state_t state;
    state.parent = NULL;
    state.layered = NULL;
    state.mode = render_mode_painting;
    state.opacity = 1.f;
    state.extents = INVALID_RECT;
    state.view_width = document->width;
    state.view_height = document->height;
    plutovg_canvas_get_matrix(canvas, &state.matrix);
    if(id == NULL) {
        state.element = document->root_element;
    } else {
        const string_t name = {id, strlen(id)};
        const element_t* element = find_element(document, &name);
        if(element == NULL)
            return false;
        state.element = element;
    }

    render_context_t context = {document, canvas, current_color, palette_func, closure, 0};
    render_element(state.element, &context, &state);
    return true;
}

plutovg_surface_t* plutosvg_document_render_to_surface(const plutosvg_document_t* document, const char* id, int width, int height, const plutovg_color_t* current_color, plutosvg_palette_func_t palette_func, void* closure)
{
    plutovg_rect_t extents = {0, 0, document->width, document->height};
    if(id && !plutosvg_document_extents(document, id, &extents))
        return NULL;
    if(extents.w <= 0.f || extents.h <= 0.f)
        return NULL;
    if(width <= 0 && height <= 0) {
        width = (int)(ceilf(extents.w));
        height = (int)(ceilf(extents.h));
    } else if(width > 0 && height <= 0) {
        height = (int)(ceilf(width * extents.h / extents.w));
    } else if(height > 0 && width <= 0) {
        width = (int)(ceilf(height * extents.w / extents.h));
    }

    plutovg_surface_t* surface = plutovg_surface_create(width, height);
    if(surface == NULL)
        return NULL;
    plutovg_canvas_t* canvas = plutovg_canvas_create(surface);
    plutovg_canvas_scale(canvas, width / extents.w, height / extents.h);
    plutovg_canvas_translate(canvas, -extents.x, -extents.y);
    if(!plutosvg_document_render(document, id, canvas, current_color, palette_func, closure)) {
        plutovg_canvas_destroy(canvas);
        plutovg_surface_destroy(surface);
        return NULL;
    }

    plutovg_canvas_destroy(canvas);
    return surface;
}

float plutosvg_document_get_width(const plutosvg_document_t* document)
{
    return document->width;
}

float plutosvg_document_get_height(const plutosvg_document_t* document)
{
    return document->height;
}

bool plutosvg_document_extents(const plutosvg_document_t* document, const char* id, plutovg_rect_t* extents)
{
    render_state_t state;
    state.parent = NULL;
    state.layered = NULL;
    state.mode = render_mode_bounding;
    state.opacity = 1.f;
    state.extents = INVALID_RECT;
    state.view_width = document->width;
    state.view_height = document->height;
    plutovg_matrix_init_identity(&state.matrix);
    if(id == NULL) {
        state.element = document->root_element;
    } else {
        const string_t name = {id, strlen(id)};
        const element_t* element = find_element(document, &name);
        if(element == NULL) {
            *extents = EMPTY_RECT;
            return false;
        }

        state.element = element;
    }

    render_context_t context = {document, NULL, NULL, NULL, NULL, 0};
    render_element(state.element, &context, &state);
    if(IS_INVALID_RECT(state.extents)) {
        *extents = EMPTY_RECT;
    } else {
        *extents = state.extents;
    }

    return true;
}

#ifdef PLUTOSVG_HAS_FREETYPE

#include "plutosvg-ft.h"

const void* plutosvg_ft_svg_hooks(void)
{
    return &plutosvg_ft_hooks;
}

#else

const void* plutosvg_ft_svg_hooks(void)
{
    return NULL;
}

#endif // PLUTOSVG_HAS_FREETYPE
