#include <stdlib.h>
#include <string.h>

#include "plistx.h"

void
px_set_str(plist_t dict, const char *key, const char *value)
{
  plist_dict_set_item(dict, key, plist_new_string(value ? value : ""));
}

void
px_set_bool(plist_t dict, const char *key, bool value)
{
  plist_dict_set_item(dict, key, plist_new_bool(value));
}

void
px_set_uint(plist_t dict, const char *key, uint64_t value)
{
  plist_dict_set_item(dict, key, plist_new_uint(value));
}

void
px_set_u64_as_int(plist_t dict, const char *key, uint64_t value)
{
  plist_dict_set_item(dict, key, plist_new_int((int64_t)value));
}

void
px_set_data(plist_t dict, const char *key, const void *data, size_t len)
{
  plist_dict_set_item(dict, key, plist_new_data(data, len));
}

void
px_set(plist_t dict, const char *key, plist_t value)
{
  plist_dict_set_item(dict, key, value);
}

int
px_to_bin(plist_t p, uint8_t **out, size_t *len)
{
  char *buf = NULL;
  uint32_t n = 0;

  if (plist_to_bin(p, &buf, &n) != PLIST_ERR_SUCCESS || !buf)
    return -1;
  *out = (uint8_t *)buf;
  *len = n;
  return 0;
}

plist_t
px_from_bin(const void *data, size_t len)
{
  plist_t p = NULL;

  if (!data || len < 8)
    return NULL;
  if (plist_from_bin(data, (uint32_t)len, &p) != PLIST_ERR_SUCCESS)
    return NULL;
  return p;
}

const char *
px_get_str(plist_t dict, const char *key)
{
  plist_t n = dict ? plist_dict_get_item(dict, key) : NULL;

  if (!n || plist_get_node_type(n) != PLIST_STRING)
    return NULL;
  return plist_get_string_ptr(n, NULL);
}

bool
px_get_uint(plist_t dict, const char *key, uint64_t *value)
{
  plist_t n = dict ? plist_dict_get_item(dict, key) : NULL;

  if (!n || plist_get_node_type(n) != PLIST_UINT)
    return false;
  plist_get_uint_val(n, value);
  return true;
}

bool
px_get_bool(plist_t dict, const char *key, bool *value)
{
  plist_t n = dict ? plist_dict_get_item(dict, key) : NULL;
  uint8_t b;

  if (!n || plist_get_node_type(n) != PLIST_BOOLEAN)
    return false;
  plist_get_bool_val(n, &b);
  *value = b;
  return true;
}

char *
px_describe(plist_t p)
{
  char *xml = NULL;
  uint32_t len = 0;

  if (!p)
    return strdup("(none)");
  plist_to_xml(p, &xml, &len);
  return xml ? xml : strdup("(unprintable)");
}
