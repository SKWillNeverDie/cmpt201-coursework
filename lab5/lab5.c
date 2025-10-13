#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

struct header {
  uint64_t size;
  struct header *next;
  int id;
};

void initialize_block(struct header *block, uint64_t size, struct header *next,
                      int id) {
  block->size = size;
  block->next = next;
  block->id = id;
}

int find_first_fit(struct header *free_list_ptr, uint64_t size) {
  struct header *curr = free_list_ptr;
  while (curr != NULL) {
    if (curr->size >= size) {
      return curr->id;
    }
    curr = curr->next;
  }
  return -1;
}

int find_best_fit(struct header *free_list_ptr, uint64_t size) {
  struct header *curr = free_list_ptr;
  int best_fit_id = -1;
  uint64_t best_size = UINT64_MAX;

  while (curr != NULL) {
    if (curr->size >= size && curr->size < best_size) {
      best_size = curr->size;
      best_fit_id = curr->id;
    }
    curr = curr->next;
  }
  return best_fit_id;
}

int find_worst_fit(struct header *free_list_ptr, uint64_t size) {
  struct header *curr = free_list_ptr;
  int worst_fit_id = -1;
  uint64_t worst_size = 0;

  while (curr != NULL) {
    if (curr->size >= size && curr->size > worst_size) {
      worst_size = curr->size;
      worst_fit_id = curr->id;
    }
    curr = curr->next;
  }
  return worst_fit_id;
}

int main(void) {
  struct header *free_block1 = (struct header *)malloc(sizeof(struct header));
  struct header *free_block2 = (struct header *)malloc(sizeof(struct header));
  struct header *free_block3 = (struct header *)malloc(sizeof(struct header));
  struct header *free_block4 = (struct header *)malloc(sizeof(struct header));
  struct header *free_block5 = (struct header *)malloc(sizeof(struct header));

  initialize_block(free_block1, 6, free_block2, 1);
  initialize_block(free_block2, 12, free_block3, 2);
  initialize_block(free_block3, 24, free_block4, 3);
  initialize_block(free_block4, 8, free_block5, 4);
  initialize_block(free_block5, 4, NULL, 5);

  struct header *free_list_ptr = free_block1;

  int first_fit_id = find_first_fit(free_list_ptr, 7);
  int best_fit_id = find_best_fit(free_list_ptr, 7);
  int worst_fit_id = find_worst_fit(free_list_ptr, 7);

  printf("The ID for First-Fit algorithm is: %d\n", first_fit_id);
  printf("The ID for Best-Fit algorithm is: %d\n", best_fit_id);
  printf("The ID for Worst-Fit algorithm is: %d\n", worst_fit_id);

  free(free_block1);
  free(free_block2);
  free(free_block3);
  free(free_block4);
  free(free_block5);

  return 0;
}

/*
prev = NULL
curr = free_list

while (curr != NULL and curr < newly_freed_block):
    prev = curr
    curr = curr->next

newly_freed_block->next = curr
if (prev == NULL)
    free_list = newly_freed_block
else
    prev->next = newly_freed_block

if (curr != NULL and end_of(newly_freed_block) == start_of(curr)):
    newly_freed_block->size += curr->size
    newly_freed_block->next = curr->next

if (prev != NULL and end_of(prev) == start_of(newly_freed_block)):
    prev->size += newly_freed_block->size
    prev->next = newly_freed_block->next
*/
