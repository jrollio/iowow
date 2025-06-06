#include "iwpool.h"
#include "iwlog.h"
#include "iwchars.h"

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>

#define IWPOOL_UNIT_ALIGN_SIZE 8UL

/** Atomic heap unit */
struct iwpool_unit {
  void *heap;
  struct iwpool_unit *next;
};


/** Memory pool */
struct iwpool {
  size_t usiz;                         /**< Used size */
  size_t asiz;                         /**< Allocated size */
  struct iwpool_unit *unit;            /**< Current heap unit */
  void *user_data;                     /**< Associated user data */
  void  (*user_data_free_fn)(void*);   /**< User data dispose function */
  int   numrefs;                       /**< Number of references if reached 0 a pool will destroyed by iwpool_destroy */
  char *heap;                          /**< Current pool heap ptr */
  struct iwpool *parent;               /**< Optional parent pool */
  struct iwpool *children;             /**< Children pools */
  struct iwpool *next;                 /**< Next child pool */
};

struct iwpool* iwpool_create(size_t siz) {
  struct iwpool *pool;
  siz = siz < 1 ? IWPOOL_POOL_SIZ : siz;
  siz = IW_ROUNDUP(siz, IWPOOL_UNIT_ALIGN_SIZE);
  pool = malloc(sizeof(*pool));
  if (!pool) {
    goto error;
  }
  pool->unit = malloc(sizeof(*pool->unit));
  if (!pool->unit) {
    goto error;
  }
  pool->unit->heap = malloc(siz);
  if (!pool->unit->heap) {
    goto error;
  }
  pool->asiz = siz;
  pool->heap = pool->unit->heap;
  pool->numrefs = 1;
  pool->usiz = 0;
  pool->unit->next = 0;
  pool->user_data = 0;
  pool->user_data_free_fn = 0;
  pool->parent = 0;
  pool->children = 0;
  pool->next = 0;

  return pool;

error:
  if (pool) {
    if (pool->unit && pool->unit->heap) {
      free(pool->unit->heap);
    }
    free(pool->unit);
    free(pool);
  }
  return 0;
}

struct iwpool* iwpool_create_attach(struct iwpool *parent, size_t siz) {
  struct iwpool *res = iwpool_create(siz);
  if (!res || !parent) {
    return res;
  }
  res->parent = parent;
  if (!parent->children) {
    parent->children = res;
  } else {
    res->next = parent->children;
    parent->children = res;
  }
  return res;
}

struct iwpool* iwpool_create_empty(void) {
  struct iwpool *ret = calloc(1, sizeof(struct iwpool));
  if (ret) {
    ret->numrefs = 1;
  }
  return ret;
}

struct iwpool* iwpool_create_empty_attach(struct iwpool *parent) {
  struct iwpool *res = iwpool_create_empty();
  if (!res || !parent) {
    return res;
  }
  res->parent = parent;
  if (!parent->children) {
    parent->children = res;
  } else {
    res->next = parent->children;
    parent->children = res;
  }
  return res;
}

IW_INLINE int iwpool_extend(struct iwpool *pool, size_t siz) {
  struct iwpool_unit *nunit = malloc(sizeof(*nunit));
  if (!nunit) {
    return 0;
  }
  siz = IW_ROUNDUP(siz, IWPOOL_UNIT_ALIGN_SIZE);
  nunit->heap = malloc(siz);
  if (!nunit->heap) {
    free(nunit);
    return 0;
  }
  nunit->next = pool->unit;
  pool->heap = nunit->heap;
  pool->unit = nunit;
  pool->usiz = 0;
  pool->asiz = siz;
  return 1;
}

void* iwpool_alloc(size_t siz, struct iwpool *pool) {
  siz = IW_ROUNDUP(siz, IWPOOL_UNIT_ALIGN_SIZE);
  size_t usiz = pool->usiz + siz;
  if (SIZE_T_MAX - pool->usiz < siz) {
    return 0;
  }
  void *h = pool->heap;
  if (usiz > pool->asiz) {
    if (SIZE_T_MAX - pool->asiz < usiz) {
      return 0;
    }
    usiz = usiz + pool->asiz;
    if (!iwpool_extend(pool, usiz)) {
      return 0;
    }
    h = pool->heap;
  }
  pool->usiz += siz;
  pool->heap += siz;
  return h;
}

void* iwpool_calloc(size_t siz, struct iwpool *pool) {
  void *res = iwpool_alloc(siz, pool);
  if (!res) {
    return 0;
  }
  memset(res, 0, siz);
  return res;
}

char* iwpool_strndup(struct iwpool *pool, const char *str, size_t len, iwrc *rcp) {
  char *ret = iwpool_alloc(len + 1, pool);
  if (!ret) {
    *rcp = iwrc_set_errno(IW_ERROR_ALLOC, errno);
    return 0;
  } else {
    *rcp = 0;
  }
  memcpy(ret, str, len);
  ret[len] = '\0';
  return ret;
}

char* iwpool_strdup(struct iwpool *pool, const char *str, iwrc *rcp) {
  return iwpool_strndup(pool, str, strlen(str), rcp);
}

char* iwpool_strdup2(struct iwpool *pool, const char *str) {
  iwrc rc;
  return iwpool_strndup(pool, str, strlen(str), &rc);
}

char* iwpool_strndup2(struct iwpool *pool, const char *str, size_t len) {
  iwrc rc;
  return iwpool_strndup(pool, str, len, &rc);
}

IW_INLINE int _iwpool_printf_estimate_size(const char *format, va_list ap) {
  char buf[1];
  return vsnprintf(buf, sizeof(buf), format, ap) + 1;
}

static char* _iwpool_printf_va(struct iwpool *pool, int size, const char *format, va_list ap) {
  char *wbuf = iwpool_alloc(size, pool);
  if (!wbuf) {
    return 0;
  }
  vsnprintf(wbuf, size, format, ap);
  return wbuf;
}

char* iwpool_printf_va(struct iwpool *pool, const char *format, va_list va) {
  va_list cva;
  va_copy(cva, va);
  int size = _iwpool_printf_estimate_size(format, va);
  va_end(va);
  char *res = _iwpool_printf_va(pool, size, format, cva);
  va_end(cva);
  return res;
}

char* iwpool_printf(struct iwpool *pool, const char *format, ...) {
  va_list ap;
  va_start(ap, format);
  int size = _iwpool_printf_estimate_size(format, ap);
  va_end(ap);
  va_start(ap, format);
  char *res = _iwpool_printf_va(pool, size, format, ap);
  va_end(ap);
  return res;
}

const char** iwpool_split_string(
  struct iwpool *pool, const char *haystack, const char *split_chars,
  bool ignore_whitespace) {
  size_t hsz = strlen(haystack);
  const char **ret = (const char**) iwpool_alloc((hsz + 1) * sizeof(char*), pool);
  if (!ret) {
    return 0;
  }
  const char *sp = haystack;
  const char *ep = sp;
  int j = 0;
  for (int i = 0; *ep; ++i, ++ep) {
    const char ch = haystack[i];
    const char *sch = strchr(split_chars, ch);
    if ((ep >= sp) && (sch || (*(ep + 1) == '\0'))) {
      if (!sch && (*(ep + 1) == '\0')) {
        ++ep;
      }
      if (ignore_whitespace) {
        while (iwchars_is_space(*sp)) ++sp;
        while (iwchars_is_space(*(ep - 1))) --ep;
      }
      if (ep >= sp) {
        char *s = iwpool_alloc(ep - sp + 1, pool);
        if (!s) {
          return 0;
        }
        memcpy(s, sp, ep - sp);
        s[ep - sp] = '\0';
        ret[j++] = s;
        ep = haystack + i;
      }
      sp = haystack + i + 1;
    }
  }
  ret[j] = 0;
  return ret;
}

const char** iwpool_copy_cstring_array(const char **v, struct iwpool *pool) {
  const char **sv = v;
  while (sv && *sv) {
    ++sv;
  }
  if (sv > v) {
    const char **ret = (const char**) iwpool_alloc(sizeof(*ret) * (sv - v + 1), pool), **wv = ret;
    if (ret) {
      for ( ; *v; ++wv, ++v) {
        *wv = iwpool_strdup2(pool, *v);
      }
      *(++wv) = 0;
    }
    return ret;
  } else {
    return 0;
  }
}

const char** iwpool_printf_split(
  struct iwpool *pool,
  const char *split_chars, bool ignore_whitespace,
  const char *format, ...) {
  va_list ap;
  va_start(ap, format);
  int size = _iwpool_printf_estimate_size(format, ap);
  va_end(ap);
  char *buf = malloc(size);
  if (!buf) {
    return 0;
  }
  va_start(ap, format);
  vsnprintf(buf, size, format, ap);
  va_end(ap);
  const char **ret = iwpool_split_string(pool, buf, split_chars, ignore_whitespace);
  free(buf);
  return ret;
}

void iwpool_free_fn(void *pool) {
  iwpool_destroy(pool);
}

void iwpool_user_data_set(struct iwpool *pool, void *data, void (*free_fn)(void*)) {
  if (pool->user_data_free_fn) {
    pool->user_data_free_fn(pool->user_data);
  }
  pool->user_data_free_fn = free_fn;
  pool->user_data = data;
}

void* iwpool_user_data_detach(struct iwpool *pool) {
  pool->user_data_free_fn = 0;
  return pool->user_data;
}

void* iwpool_user_data_get(struct iwpool *pool) {
  return pool->user_data;
}

size_t iwpool_allocated_size(struct iwpool *pool) {
  return pool->asiz;
}

size_t iwpool_used_size(struct iwpool *pool) {
  return pool->usiz;
}

int iwpool_ref(struct iwpool *pool) {
  return ++pool->numrefs;
}

static void _parent_remove_child(struct iwpool *parent, struct iwpool *child) {
  for (struct iwpool *c = parent->children, *p = 0; c; p = c, c = c->next) {
    if (c == child) {
      c->parent = 0;
      if (p) {
        p->next = c->next;
      } else {
        parent->children = 0;
      }
      break;
    }
  }
}

bool iwpool_destroy(struct iwpool *pool) {
  if (!pool || --pool->numrefs > 0) {
    return false;
  }
  if (pool->parent) {
    _parent_remove_child(pool->parent, pool);
  }
  for (struct iwpool *c = pool->children; c; c = c->next) {
    c->parent = 0;
    iwpool_destroy(c);
  }
  for (struct iwpool_unit *u = pool->unit, *next; u; u = next) {
    next = u->next;
    free(u->heap);
    free(u);
  }
  if (pool->user_data_free_fn) {
    pool->user_data_free_fn(pool->user_data);
  }
  free(pool);
  return true;
}
