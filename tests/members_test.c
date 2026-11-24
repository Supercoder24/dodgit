/* Host test of the ball's table of pouches (shared/members.h):
 *   cc -std=c11 -Wall -Wextra -Werror -Ishared tests/members_test.c -o /tmp/members_test && /tmp/members_test */
#include <assert.h>
#include <stdio.h>
#include "members.h"

static void test_join_repeat_full(void)
{
  struct members t;
  int slot = -2;

  members_init(&t);
  assert(t.count == 0 && t.highest == -1 && members_mask(&t) == 0);

  /* first come, first served: lowest free slot, full lives */
  assert(members_join(&t, 0xA, &slot) == JOIN_NEW && slot == 0);
  assert(members_join(&t, 0xB, &slot) == JOIN_NEW && slot == 1);
  assert(t.count == 2 && t.highest == 1 && members_mask(&t) == 0x03);
  assert(t.lives[0] == LIVES_MAX && t.lives[1] == LIVES_MAX);

  /* a repeated JOIN (rebooted pouch, lost answer) keeps slot AND lives */
  assert(members_hit(&t, 0) == LIVES_MAX - 1);
  assert(members_join(&t, 0xA, &slot) == JOIN_REPEAT && slot == 0);
  assert(t.count == 2 && t.lives[0] == LIVES_MAX - 1);

  /* table full: the next one is refused and nothing changes */
  for (uint32_t id = 0x100; t.count < MAX_POUCHES; id++)
  {
    assert(members_join(&t, id, &slot) == JOIN_NEW);
  }
  assert(t.highest == MAX_POUCHES - 1 && members_mask(&t) == 0xFF);
  assert(members_join(&t, 0xDEAD, &slot) == JOIN_FULL && slot == -1);
  assert(t.count == MAX_POUCHES);
  assert(members_find(&t, 0xDEAD) == -1 && members_find(&t, 0xB) == 1);
}

static void test_delete_reuse_highest(void)
{
  struct members t;
  int slot;

  members_init(&t);
  for (uint32_t id = 1; id <= 4; id++)
  {
    members_join(&t, id, &slot);
  }
  assert(t.highest == 3);

  /* deleting the highest slot shortens the ranging round; a hole below keeps the highest */
  assert(members_delete(&t, 3) && t.highest == 2 && t.count == 3);
  assert(members_delete(&t, 1) && t.highest == 2 && t.count == 2 && members_mask(&t) == 0x05);
  assert(members_delete(&t, 2) && t.highest == 0);
  assert(members_delete(&t, 0) && t.highest == -1 && t.count == 0);
  assert(!members_delete(&t, 0) && !members_delete(&t, -1) && !members_delete(&t, MAX_POUCHES));

  /* freed slots are reused lowest first, with fresh lives */
  members_join(&t, 10, &slot);
  members_join(&t, 11, &slot);
  members_hit(&t, 0);
  members_delete(&t, 0);
  assert(members_join(&t, 12, &slot) == JOIN_NEW && slot == 0 && t.lives[0] == LIVES_MAX);
  assert(members_join(&t, 11, &slot) == JOIN_REPEAT && slot == 1);
}

static void test_lives_and_rounds(void)
{
  struct members t;
  int slot;

  members_init(&t);
  members_join(&t, 1, &slot);
  assert(members_hit(&t, 5) == -1); /* empty slot */
  for (int i = LIVES_MAX - 1; i >= 0; i--)
  {
    assert(members_hit(&t, 0) == i);
  }
  assert(members_hit(&t, 0) == 0); /* never below 0 */

  members_join(&t, 2, &slot);
  members_ack(&t, 0);
  members_ack(&t, 7); /* not in use: ignored */
  assert(members_acked_mask(&t) == 0x01);
  members_new_round(&t); /* start of a Reset */
  assert(t.lives[0] == LIVES_MAX && t.lives[1] == LIVES_MAX && members_acked_mask(&t) == 0);

  /* a deleted pouch is no longer acked */
  members_ack(&t, 1);
  members_delete(&t, 1);
  assert(members_acked_mask(&t) == 0);
}

static void test_remember_round_robin(void)
{
  struct members t;
  int slot;

  members_init(&t);
  assert(members_next_remember(&t) == -1);

  members_join(&t, 1, &slot);
  assert(members_next_remember(&t) == 0 && members_next_remember(&t) == 0);

  members_join(&t, 2, &slot);
  members_join(&t, 3, &slot);
  int seen[MAX_POUCHES] = {0};
  for (int i = 0; i < 6; i++)
  {
    seen[members_next_remember(&t)]++;
  }
  assert(seen[0] == 2 && seen[1] == 2 && seen[2] == 2); /* each pouch once per cycle */

  /* deleting and joining between calls never skips or repeats a pouch within a cycle */
  members_delete(&t, 1);
  int a = members_next_remember(&t), b = members_next_remember(&t);
  assert(a != b && a != 1 && b != 1);
  members_join(&t, 4, &slot); /* takes the hole at slot 1 */
  assert(slot == 1);
  int order[3] = {members_next_remember(&t), members_next_remember(&t), members_next_remember(&t)};
  assert(order[0] != order[1] && order[1] != order[2] && order[0] != order[2]);
}

/* The Reset window: lives back to full, pouches that answer or join are kept, silent ones are deleted
 * (their slots freed, ranging rounds shortened), a pouch that joins in the very last moment is not. */
static void test_reset_window(void)
{
  struct members t;
  int slot;

  members_init(&t);
  for (uint32_t id = 1; id <= 4; id++)
  {
    members_join(&t, id, &slot);
  }
  members_hit(&t, 1);
  members_hit(&t, 3);

  members_new_round(&t);
  assert(t.lives[1] == LIVES_MAX && t.lives[3] == LIVES_MAX && members_unacked_mask(&t) == 0x0F);

  members_ack(&t, 0);  /* answered a RESET */
  members_ack(&t, 2);
  assert(members_join(&t, 5, &slot) == JOIN_NEW && slot == 4);
  members_ack(&t, slot); /* a JOIN counts as an acknowledgement */
  assert(members_join(&t, 1, &slot) == JOIN_REPEAT && slot == 0);
  members_ack(&t, slot);
  assert(members_unacked_mask(&t) == 0x0A); /* slots 1 and 3 stayed silent */

  uint8_t silent = members_unacked_mask(&t);
  for (int s2 = 0; s2 < MAX_POUCHES; s2++)
  {
    if (silent & (1U << s2))
    {
      assert(members_delete(&t, s2));
    }
  }
  assert(t.count == 3 && members_mask(&t) == 0x15 && t.highest == 4 && members_unacked_mask(&t) == 0);

  /* a late pouch takes the lowest freed slot, the silent ones can come back as new players */
  assert(members_join(&t, 6, &slot) == JOIN_NEW && slot == 1);
  assert(members_join(&t, 2, &slot) == JOIN_NEW && slot == 3);

  /* everybody silent: the table empties and the next round starts from slot 0 */
  members_new_round(&t);
  for (int s2 = 0; s2 < MAX_POUCHES; s2++)
  {
    members_delete(&t, s2);
  }
  assert(t.count == 0 && t.highest == -1 && members_mask(&t) == 0);
  assert(members_join(&t, 99, &slot) == JOIN_NEW && slot == 0);
}

int main(void)
{
  test_join_repeat_full();
  test_delete_reuse_highest();
  test_lives_and_rounds();
  test_remember_round_robin();
  test_reset_window();
  printf("members_test: OK\n");
  return 0;
}
