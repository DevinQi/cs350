#include <types.h>
#include <lib.h>
#include <synchprobs.h>
#include <synch.h>

/* 
 * Code Changed
 */

static struct lock *gLock;
static struct cv *cat_cv;
static struct cv *mouse_cv;
static int totalBowls;

static volatile bool mouseTurn;
static volatile int slotsLeft;
static volatile int bowlsUsed;

static volatile int numCatWaiting;
static volatile int numMouseWaiting;
static volatile bool *util;

void switch_kind_to(bool mouse);
void any_before_eating(unsigned int bowl, bool mouse);
void any_after_eating(unsigned int bowl, bool mouse);

void
catmouse_sync_init(int bowls)
{
  gLock = lock_create("globalCatMouseLock");
  if (gLock == NULL) {
    panic("could not create global CatMouse synchronization lock");
  }

  cat_cv = cv_create("cat_cv");
  if (cat_cv == NULL) {
    panic("could not create cat cv");
  }

  mouse_cv = cv_create("mouse_cv");
  if (mouse_cv == NULL) {
    panic("could not create mouse cv");
  }

  totalBowls = bowls;
  slotsLeft = 2 * bowls;
  mouseTurn = true;
  util = kmalloc(bowls * sizeof(bool));
  for(int i = 0; i < bowls; i++) {
    util[i] = false;
  }
  numCatWaiting = 0;
  numMouseWaiting = 0;

  return;
}

void
catmouse_sync_cleanup(int bowls)
{
  (void) bowls;

  KASSERT(gLock != NULL);
  KASSERT(cat_cv != NULL);
  KASSERT(mouse_cv != NULL);

  lock_destroy(gLock);
  cv_destroy(cat_cv);
  cv_destroy(mouse_cv);

  if(util != NULL) {
    kfree( (void *) util);
  }
}

void
switch_kind_to(bool mouse) {
  // Trying to switch to mouse
  if(mouse) {
    // Only switch if there are mice waiting
    mouse = numMouseWaiting > 0;
  }
  // Trying to switch to cat
  else {
    // Only switch if there are cats waiting
    mouse = !(numCatWaiting > 0);
  }

  // Reset slots left
  slotsLeft = 2 * totalBowls;
  // Switch to other creature
  mouseTurn = mouse;
  // Wake up all creatures of the correct type
  struct cv *cdn = mouse ? mouse_cv : cat_cv;
  cv_broadcast(cdn, gLock);
}

void
any_before_eating(unsigned int bowl, bool mouse) {
  KASSERT(gLock != NULL);
  lock_acquire(gLock);

  // Find condition variable
  struct cv *cdn = mouse ? mouse_cv : cat_cv;
  // Increase waiting
  if(mouse) {
    numMouseWaiting++;
  }
  else {
    numCatWaiting++;
  }

  // Wait until all conditions are satisfied
  bool ready = false;
  while(!ready) {
    ready = true;
    // If not the creature's turn
    if(mouse != mouseTurn) {
      // If nobody eating, switch to the creature
      if(bowlsUsed == 0) {
        switch_kind_to(mouse);
      }
      else {
        ready = false;
      }
    }
    // If creatures already ate here
    if(slotsLeft <= 0) {
      ready = false;
    }
    // If bowl is occupied
    if(util[bowl-1]) {
      ready = false;
    }

    if(!ready) {
      cv_wait(cdn, gLock);
    }
  }

  KASSERT(mouse == mouseTurn);
  KASSERT(slotsLeft > 0);
  KASSERT(!util[bowl-1]);

  // Take the bowl
  util[bowl-1] = true;
  bowlsUsed++;
  // Use a slot
  slotsLeft--;

  // Decrease waiting
  if(mouse) {
    numMouseWaiting--;
  }
  else {
    numCatWaiting--;
  }

  lock_release(gLock);  
}

void
any_after_eating(unsigned int bowl, bool mouse) {
  KASSERT(gLock != NULL);
  lock_acquire(gLock);

  KASSERT(mouse == mouseTurn);
  KASSERT(bowlsUsed > 0);
  KASSERT(util[bowl-1]);

  // Free bowl
  util[bowl-1] = false;
  bowlsUsed--;

  // Switch kind if all bowls are free
  if(bowlsUsed == 0) {
    switch_kind_to(!mouse);
  }

  lock_release(gLock);  
}

void
cat_before_eating(unsigned int bowl) 
{
  any_before_eating(bowl, false);
}

void
cat_after_eating(unsigned int bowl) 
{
  any_after_eating(bowl, false);
}

void
mouse_before_eating(unsigned int bowl) 
{
  any_before_eating(bowl, true);
}

void
mouse_after_eating(unsigned int bowl) 
{
  any_after_eating(bowl, true);
}
