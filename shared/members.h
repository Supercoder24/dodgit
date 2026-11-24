/*
 * dodgit - the ball's table of pouches (pure C, no Zephyr: host test in tests/members_test.c).
 *
 * The ball is the source of truth for who is in the game: per timeslot it stores the pouch's
 * 32-bit hardware ID and its remaining lives. The table lives in RAM only, so a ball reboot
 * starts a new game (the pouches notice and re-register, see shared/README.md).
 *
 * `acked` is a per-Reset flag: a pouch that answered the Reset (RESET_ACK) or joined during it
 * is kept, one that did neither is deleted when the Reset window closes.
 */
#ifndef DODGIT_MEMBERS_H
#define DODGIT_MEMBERS_H

#include <stdbool.h>
#include <stdint.h>
#include "settings.h"

enum join_result
{
  JOIN_NONE,   /* nothing (valid) heard */
  JOIN_NEW,    /* new pouch registered in the lowest free timeslot */
  JOIN_REPEAT, /* known pouch asked again (reboot, or its answer was lost): same timeslot */
  JOIN_FULL,   /* table full: ignored */
};

struct members
{
  uint32_t uid[MAX_POUCHES];
  uint8_t lives[MAX_POUCHES];
  bool used[MAX_POUCHES];
  bool acked[MAX_POUCHES];
  int count;         /* used slots */
  int highest;       /* highest used slot, -1 if none (ranging rounds listen through it) */
  int remember_next; /* round-robin cursor of members_next_remember() */
};

static inline void members_init(struct members *t)
{
  *t = (struct members){.highest = -1};
}

/* Slot of the pouch with this hardware ID, or -1. */
static inline int members_find(const struct members *t, uint32_t uid)
{
  for (int i = 0; i < MAX_POUCHES; i++)
  {
    if (t->used[i] && t->uid[i] == uid)
    {
      return i;
    }
  }
  return -1;
}

/* Register a pouch: a known ID keeps its slot and lives (JOIN_REPEAT), a new one gets the lowest
 * free slot ("first come, first served") with full lives (JOIN_NEW). *slot is -1 if the table is
 * full (JOIN_FULL). */
static inline enum join_result members_join(struct members *t, uint32_t uid, int *slot)
{
  int s = members_find(t, uid);

  if (s >= 0)
  {
    *slot = s;
    return JOIN_REPEAT;
  }
  for (int i = 0; i < MAX_POUCHES; i++)
  {
    if (!t->used[i])
    {
      t->used[i] = true;
      t->uid[i] = uid;
      t->lives[i] = LIVES_MAX;
      t->acked[i] = false;
      t->count++;
      if (i > t->highest)
      {
        t->highest = i;
      }
      *slot = i;
      return JOIN_NEW;
    }
  }
  *slot = -1;
  return JOIN_FULL;
}

/* Forget a pouch: frees its slot (a later JOIN can reuse it) and recomputes the highest used
 * slot, which shortens the ranging rounds. Returns false if the slot was not in use. */
static inline bool members_delete(struct members *t, int slot)
{
  if (slot < 0 || slot >= MAX_POUCHES || !t->used[slot])
  {
    return false;
  }
  t->used[slot] = false;
  t->acked[slot] = false;
  t->count--;
  t->highest = -1;
  for (int i = 0; i < MAX_POUCHES; i++)
  {
    if (t->used[i])
    {
      t->highest = i;
    }
  }
  return true;
}

/* Bit n set = slot n in use. */
static inline uint8_t members_mask(const struct members *t)
{
  uint8_t mask = 0;
  for (int i = 0; i < MAX_POUCHES; i++)
  {
    if (t->used[i])
    {
      mask |= 1U << i;
    }
  }
  return mask;
}

/* Bit n set = slot n is in use and has acknowledged the current Reset (or joined during it). */
static inline uint8_t members_acked_mask(const struct members *t)
{
  uint8_t mask = 0;
  for (int i = 0; i < MAX_POUCHES; i++)
  {
    if (t->used[i] && t->acked[i])
    {
      mask |= 1U << i;
    }
  }
  return mask;
}

static inline void members_ack(struct members *t, int slot)
{
  if (slot >= 0 && slot < MAX_POUCHES && t->used[slot])
  {
    t->acked[slot] = true;
  }
}

/* Start of a Reset: everybody back to full lives, nobody has acknowledged yet. */
static inline void members_new_round(struct members *t)
{
  for (int i = 0; i < MAX_POUCHES; i++)
  {
    t->lives[i] = LIVES_MAX;
    t->acked[i] = false;
  }
}

/* The pouch in `slot` was hit: one life less (never below 0). Returns the lives remaining, or -1
 * if the slot is not in use. Done on the ball immediately, whether or not the pouch ever
 * acknowledges the HIT message. */
static inline int members_hit(struct members *t, int slot)
{
  if (slot < 0 || slot >= MAX_POUCHES || !t->used[slot])
  {
    return -1;
  }
  if (t->lives[slot] > 0)
  {
    t->lives[slot]--;
  }
  return t->lives[slot];
}

/* Round-robin over the used slots, for the REMEMBER trickle: every pouch comes up once per cycle,
 * also while pouches join or get deleted. Returns -1 if the table is empty. */
static inline int members_next_remember(struct members *t)
{
  for (int n = 0; n < MAX_POUCHES; n++)
  {
    int i = (t->remember_next + n) % MAX_POUCHES;
    if (t->used[i])
    {
      t->remember_next = (i + 1) % MAX_POUCHES;
      return i;
    }
  }
  return -1;
}

#endif
