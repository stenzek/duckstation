#include "rc_value.h"

#include "rc_alloc.h"
#include "rc_condition.h"
#include "rc_condset.h"
#include "rc_error.h"
#include "rc_eval_state.h"
#include "rc_operand.h"
#include "rc_operator.h"
#include "rc_parse_state.h"
#include "rc_typed_value.h"

#include <string.h> /* memset */
#include <ctype.h> /* isdigit */

typedef struct rc_value_with_memrefs_t {
  rc_value_t value;
  rc_memrefs_t memrefs;
} rc_value_with_memrefs_t;

static int rc_is_valid_variable_character(char ch, int is_first) {
  if (is_first) {
    if (!isalpha((unsigned char)ch))
      return 0;
  }
  else {
    if (!isalnum((unsigned char)ch))
      return 0;
  }
  return 1;
}

int rc_value_get_variable_name(char buffer[], size_t buffer_size, const char** memaddr) {
  const char* aux = *memaddr;
  size_t i = 0;
  char ch;

  if (buffer_size == 0)
    return 0;

  if (!rc_is_valid_variable_character(ch = *aux, 1))
    return 0;

  buffer_size--; /* leave space for terminator */
  do {
    if (i == buffer_size)
      break;

    buffer[i++] = ch;
    ch = *(++aux);
  } while (rc_is_valid_variable_character(ch, 0));

  buffer[i] = '\0';
  *memaddr = aux;
  return 1;
}

static void rc_parse_cond_value(rc_value_t* self, const char** memaddr, rc_parse_state_t* parse) {
  rc_condset_t** next_clause;

  next_clause = &self->conditions;

  do
  {
    parse->measured_target = 0; /* passing is_value=1 should prevent any conflicts, but clear it out anyway */
    *next_clause = rc_parse_condset(memaddr, parse);
    if (parse->offset < 0) {
      return;
    }

    if (**memaddr == 'S' || **memaddr == 's') {
      /* alt groups not supported */
      parse->offset = RC_INVALID_VALUE_FLAG;
    }
    else if (parse->measured_target == 0) {
      parse->offset = RC_MISSING_VALUE_MEASURED;
    }
    else if (**memaddr == '$') {
      /* maximum of */
      ++(*memaddr);
      next_clause = &(*next_clause)->next;
      continue;
    }

    break;
  } while (1);

  (*next_clause)->next = 0;
}

static void rc_parse_legacy_value(rc_value_t* self, const char** memaddr, rc_parse_state_t* parse) {
  rc_condition_t** next;
  rc_condset_t** next_clause;
  rc_condset_t* condset;
  rc_condition_t local_cond;
  rc_condition_t* cond;
  uint32_t num_measured_conditions;
  char buffer[64] = "A:";
  const char* buffer_ptr;
  char* ptr;
  int done;

  /* convert legacy format into condset */
  next_clause = &self->conditions;
  do {
    /* count the number of joiners and add one to determine the number of clauses.  */
    buffer[0] = 'A'; /* reset to AddSource */
    done = 0;
    num_measured_conditions = 1;
    buffer_ptr = *memaddr;
    do {
      switch (*buffer_ptr++) {
        case '_': /* add next */
          ++num_measured_conditions;
          buffer[0] = 'A'; /* reset to AddSource */
          break;

        case '*': /* multiply */
          if (*buffer_ptr == '-') {
            /* multiplication by a negative number will convert to SubSource */
            ++buffer_ptr;
            buffer[0] = 'B';
          }
          break;

        case '\0': /* end of string */
        case '$': /* maximum of */
        case ':': /* end of leaderboard clause */
        case ')': /* end of rich presence macro */
          done = 1;
          break;

        default: /* assume everything else is valid - bad stuff will be filtered out later */
          break;
      }
    } while (!done);

    /* if last condition is not AddSource, we'll need to add a dummy condition for the Measured */
    if (buffer[0] != 'A')
      ++num_measured_conditions;

    condset = rc_alloc_condset(num_measured_conditions, parse);
    if (!condset || parse->offset < 0)
      return;

    memset(condset, 0, sizeof(*condset));
    condset->num_measured_conditions = (uint16_t)num_measured_conditions;
    cond = rc_condset_get_conditions(condset);

    next = &condset->conditions;

    for (;; ++(*memaddr)) {
      buffer[0] = 'A'; /* reset to AddSource */
      ptr = &buffer[2];

      /* extract the next clause */
      for (;; ++(*memaddr)) {
        if (ptr == &buffer[sizeof(buffer)]) {
          /* ran out of local buffer space for converting the condition */
          parse->offset = RC_INVALID_VALUE;
          return;
        }

        switch (**memaddr) {
          case '_': /* add next */
            *ptr = '\0';
            break;

          case '$': /* maximum of */
          case '\0': /* end of string */
          case ':': /* end of leaderboard clause */
          case ')': /* end of rich presence macro */
            /* the last condition needs to be Measured - AddSource can be changed here,
             * SubSource will be handled later */
            if (buffer[0] == 'A')
              buffer[0] = 'M';

            *ptr = '\0';
            break;

          case '*':
            *ptr++ = '*';

            buffer_ptr = *memaddr + 1;
            if (*buffer_ptr == '-') {
              buffer[0] = 'B'; /* change to SubSource */
              ++(*memaddr); /* don't copy sign */
              ++buffer_ptr; /* ignore sign when doing floating point check */
            }
            else if (*buffer_ptr == '+') {
              ++buffer_ptr; /* ignore sign when doing floating point check  */
            }

            /* if it looks like a floating point number, add the 'f' prefix */
            while (isdigit((unsigned char)*buffer_ptr))
              ++buffer_ptr;
            if (*buffer_ptr == '.') {
              if (ptr == &buffer[sizeof(buffer)]) {
                parse->offset = RC_INVALID_VALUE;
                return;
              }

              *ptr++ = 'f';
            }
            continue;

          default:
            *ptr++ = **memaddr;
            continue;
        }

        break;
      }

      /* process the clause */
      if (!parse->buffer)
        cond = &local_cond;

      buffer_ptr = buffer;
      rc_parse_condition_internal(cond, &buffer_ptr, parse);
      if (parse->offset < 0)
        return;

      if (*buffer_ptr) {
        /* whatever we copied as a single condition was not fully consumed */
        parse->offset = RC_INVALID_VALUE;
        return;
      }

      if (cond->type == RC_CONDITION_MEASURED && !rc_operator_is_modifying(cond->oper)) {
        /* ignore non-modifying operator on measured clause. if it were parsed as an AddSource
         * or SubSource, that would have already happened in rc_parse_condition_internal, and
         * legacy formatted values are essentially a series of AddSources. */
        cond->oper = RC_OPERATOR_NONE;
      }

      rc_condition_update_parse_state(cond, parse);

      *next = cond;
      next = &cond->next;

      if (**memaddr != '_') /* add next */
        break;

      ++cond;
    }

    /* -- end of clause -- */

    /* clause must end in a Measured. if it doesn't, append one */
    if (cond->type != RC_CONDITION_MEASURED) {
      if (!parse->buffer)
        cond = &local_cond;
      else
        ++cond;

      buffer_ptr = "M:0";
      rc_parse_condition_internal(cond, &buffer_ptr, parse);
      *next = cond;
      next = &cond->next;
      rc_condition_update_parse_state(cond, parse);
    }

    *next = NULL;

    /* finalize clause */
    *next_clause = condset;
    next_clause = &condset->next;

    if (**memaddr != '$') {
      /* end of valid string */
      *next_clause = NULL;
      break;
    }

    /* max of ($), start a new clause */
    ++(*memaddr);
  } while (1);
}

void rc_parse_value_internal(rc_value_t* self, const char** memaddr, rc_parse_state_t* parse) {
  const uint8_t was_value = parse->is_value;
  const rc_condition_t* condition;
  parse->is_value = 1;

  /* if it starts with a condition flag (M: A: B: C:), parse the conditions */
  if ((*memaddr)[1] == ':')
    rc_parse_cond_value(self, memaddr, parse);
  else
    rc_parse_legacy_value(self, memaddr, parse);

  if (parse->offset >= 0 && parse->buffer) {
    self->name = "(unnamed)";
    self->value.value = self->value.prior = 0;
    self->value.memref_type = RC_MEMREF_TYPE_VALUE;
    self->value.changed = 0;
    self->has_memrefs = 0;

    for (condition = self->conditions->conditions; condition; condition = condition->next) {
      if (condition->type == RC_CONDITION_MEASURED) {
        if (rc_operand_is_float(&condition->operand1)) {
          self->value.size = RC_MEMSIZE_FLOAT;
          self->value.type = RC_VALUE_TYPE_FLOAT;
        }
        else {
          self->value.size = RC_MEMSIZE_32_BITS;
          self->value.type = RC_VALUE_TYPE_UNSIGNED;
        }
        break;
      }
    }
  }

  parse->is_value = was_value;
}

int rc_value_size(const char* memaddr) {
  rc_value_with_memrefs_t* value;
  rc_preparse_state_t preparse;
  rc_init_preparse_state(&preparse);

  value = RC_ALLOC(rc_value_with_memrefs_t, &preparse.parse);
  if (value)
    rc_parse_value_internal(&value->value, &memaddr, &preparse.parse);
  rc_preparse_alloc_memrefs(NULL, &preparse);

  rc_destroy_preparse_state(&preparse);
  return preparse.parse.offset;
}

rc_value_t* rc_parse_value(void* buffer, const char* memaddr, void* unused_L, int unused_funcs_idx) {
  rc_value_with_memrefs_t* value;
  rc_preparse_state_t preparse;
  const char* preparse_memaddr = memaddr;

  (void)unused_L;
  (void)unused_funcs_idx;

  if (!buffer || !memaddr)
    return NULL;

  rc_init_preparse_state(&preparse);
  value = RC_ALLOC(rc_value_with_memrefs_t, &preparse.parse);
  if (value)
    rc_parse_value_internal(&value->value, &preparse_memaddr, &preparse.parse);
  rc_preparse_alloc_memrefs(NULL, &preparse); /* allocate space for the needed memrefs */

  rc_reset_parse_state(&preparse.parse, buffer, (size_t)preparse.parse.offset);
  value = RC_ALLOC(rc_value_with_memrefs_t, &preparse.parse);
  rc_preparse_alloc_memrefs(&value->memrefs, &preparse);

  rc_parse_value_internal(&value->value, &memaddr, &preparse.parse);
  value->value.has_memrefs = 1;

  rc_destroy_preparse_state(&preparse);
  return (preparse.parse.offset >= 0) ? &value->value : NULL;
}

static void rc_update_value_memrefs(rc_value_t* self, rc_eval_state_t* eval_state) {
  if (self->has_memrefs) {
    rc_value_with_memrefs_t* value = (rc_value_with_memrefs_t*)self;
    rc_update_memref_values(&value->memrefs, eval_state);
  }
}

rc_memrefs_t* rc_value_get_memrefs(rc_value_t* self) {
  if (self->has_memrefs) {
    rc_value_with_memrefs_t* value = (rc_value_with_memrefs_t*)self;
    return &value->memrefs;
  }

  return NULL;
}

struct rc_value_t* rc_alloc_value_with_memrefs(struct rc_parse_state_t* parse) {
  rc_value_with_memrefs_t* value = RC_ALLOC(rc_value_with_memrefs_t, parse);
  value->value.has_memrefs = 1;
  return &value->value;
}

int rc_evaluate_value_typed(rc_value_t* self, rc_typed_value_t* result, rc_eval_state_t* eval_state) {
  rc_condset_t* condset;
  int valid = 0;

  rc_update_value_memrefs(self, eval_state);

  /* reset the trigger processing state before processing the value.
   * condition processing state will be reset by test_condset. */
  eval_state->has_hits = 0;
  eval_state->was_reset = 0;
  eval_state->was_cond_reset = 0;

  result->value.i32 = 0;
  result->type = RC_VALUE_TYPE_SIGNED;

  for (condset = self->conditions; condset != NULL; condset = condset->next) {
    rc_test_condset(condset, eval_state);

    if (condset->is_paused)
      continue;

    if (eval_state->was_reset) {
      /* if any ResetIf condition was true, reset the hit counts
       * NOTE: ResetIf only affects the current condset when used in values!
       */
      rc_reset_condset(condset);
    }

    if (eval_state->measured_value.type != RC_VALUE_TYPE_NONE) {
      if (!valid) {
        /* capture the first valid measurement, which may be negative */
        memcpy(result, &eval_state->measured_value, sizeof(*result));
        valid = 1;
      }
      else {
        /* multiple condsets are currently only used for the MAX_OF operation.
         * only keep the condset's value if it's higher than the current highest value.
         */
        if (rc_typed_value_compare(&eval_state->measured_value, result, RC_OPERATOR_GT))
          memcpy(result, &eval_state->measured_value, sizeof(*result));
      }
    }
  }

  return valid;
}

int32_t rc_evaluate_value(rc_value_t* self, rc_eval_state_t* eval_state) {
  rc_typed_value_t result;
  int valid = rc_evaluate_value_typed(self, &result, eval_state);

  if (valid) {
    /* if not paused, store the value so that it's available when paused. */
    rc_typed_value_convert(&result, RC_VALUE_TYPE_UNSIGNED);
    rc_update_memref_value(&self->value, result.value.u32);
  }
  else {
    /* when paused, the Measured value will not be captured, use the last captured value. */
    result.value.u32 = self->value.value;
    result.type = RC_VALUE_TYPE_UNSIGNED;
  }

  rc_typed_value_convert(&result, RC_VALUE_TYPE_SIGNED);
  return result.value.i32;
}

void rc_reset_value(rc_value_t* self) {
  rc_condset_t* condset = self->conditions;
  while (condset != NULL) {
    rc_reset_condset(condset);
    condset = condset->next;
  }

  self->value.value = self->value.prior = 0;
  self->value.changed = 0;
}

int rc_value_from_hits(rc_value_t* self)
{
  rc_condset_t* condset = self->conditions;
  for (; condset != NULL; condset = condset->next) {
    rc_condition_t* condition = condset->conditions;
    for (; condition != NULL; condition = condition->next) {
      if (condition->type == RC_CONDITION_MEASURED)
        return (condition->required_hits != 0);
    }
  }

  return 0;
}

rc_value_t* rc_alloc_variable(const char* memaddr, size_t memaddr_len, rc_parse_state_t* parse) {
  rc_value_t** value_ptr = parse->variables;
  rc_value_t* value;
  const char* name;
  uint32_t measured_target;

  if (!value_ptr)
    return NULL;

  while (*value_ptr) {
    value = *value_ptr;
    if (strncmp(value->name, memaddr, memaddr_len) == 0 && value->name[memaddr_len] == 0)
      return value;

    value_ptr = &value->next;
  }

  /* capture name before calling parse as parse will update memaddr pointer */
  name = rc_alloc_str(parse, memaddr, memaddr_len);
  if (!name)
    return NULL;

  /* no match found, create a new entry */
  value = RC_ALLOC_SCRATCH(rc_value_t, parse);
  if (!value)
    return NULL;

  memset(value, 0, sizeof(*value));
  value->value.size = RC_MEMSIZE_VARIABLE;
  value->next = NULL;

  /* the helper variable likely has a Measured condition. capture the current measured_target so we can restore it
   * after generating the variable so the variable's Measured target doesn't conflict with the rest of the trigger. */
  measured_target = parse->measured_target;
  rc_parse_value_internal(value, &memaddr, parse);
  parse->measured_target = measured_target;

  /* store name after calling parse as parse will set name to (unnamed) */
  value->name = name;

  *value_ptr = value;
  return value;
}

uint32_t rc_count_values(const rc_value_t* values) {
  uint32_t count = 0;
  while (values) {
    ++count;
    values = values->next;
  }

  return count;
}

void rc_update_values(rc_value_t* values, rc_eval_state_t* eval_state) {
  rc_typed_value_t result;

  rc_value_t* value = values;
  for (; value; value = value->next) {
    if (rc_evaluate_value_typed(value, &result, eval_state)) {
      /* store the raw bytes and type to be restored by rc_typed_value_from_memref_value  */
      rc_update_memref_value(&value->value, result.value.u32);
      value->value.type = result.type;
    }
  }
}

void rc_reset_values(rc_value_t* values) {
  rc_value_t* value = values;

  for (; value; value = value->next)
    rc_reset_value(value);
}
