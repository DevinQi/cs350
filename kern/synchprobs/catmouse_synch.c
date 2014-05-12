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
static volatile int slotsUsed;

static volatile int numCatWaiting;
static volatile int numMouseWaiting;
static volatile bool *util;

void switch_kind_to(bool mouse);
void any_before_eating(unsigned int bowl, bool mouse);
void any_after_eating(unsigned int bowl, bool mouse);


/* 
 * The CatMouse simulation will call this function once before any cat or
 * mouse tries to each.
 *
 * You can use it to initialize synchronization and other variables.
 * 
 * parameters: the number of bowls
 */
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
  slotsLeft = totalBowls;
  mouseTurn = false;
  util = kmalloc(bowls*sizeof(bool));
  for(int i = 0; i < bowls; i++) {
    util[i] = false;
  }
  numCatWaiting = 0;
  numMouseWaiting = 0;

  return;
}

/* 
 * The CatMouse simulation will call this function once after all cat
 * and mouse simulations are finished.
 *
 * You can use it to clean up any synchronization and other variables.
 *
 * parameters: the number of bowls
 */
void
catmouse_sync_cleanup(int bowls)
{
  (void) bowls;
  KASSERT(gLock != NULL);
  lock_destroy(gLock);
  cv_destroy(cat_cv);
  cv_destroy(mouse_cv);
  if(util != NULL) {
    kfree( (void *) util);
  }
}

void switch_kind_to(bool mouse) {
  struct cv *condition;
  if(mouse) {
    if(numMouseWaiting > 0) {
      mouse = true;
    }
    else {
      mouse = false;
    }
  }
  else {
    if(numCatWaiting > 0) {
      mouse = false;
    }
    else {
      mouse = true;
    }
  }


  if(mouse) {
    condition = mouse_cv;
  }
  else {
    condition = cat_cv;
  }
  slotsLeft = totalBowls;
  mouseTurn = mouse;
  cv_broadcast(condition, gLock);
}

void any_before_eating(unsigned int bowl, bool mouse) {
  KASSERT(gLock != NULL);
  lock_acquire(gLock);

  struct cv *condition;
  if(mouse) {
    numMouseWaiting++;
    condition = mouse_cv;
  }
  else {
    numCatWaiting++;
    condition = cat_cv;
  }

  bool ready = false;

  while(!ready) {
    ready = true;
    if(mouse != mouseTurn) {
      //If nobody eating, switch
      if(slotsUsed == 0) {
        switch_kind_to(mouse);
      }
      else {
        ready = false;
      }
    }
    if(slotsLeft <= 0) {
      ready = false;
    }
    if(util[bowl-1]) {
      ready = false;
    }

    if(!ready) {
      cv_wait(condition, gLock);
    }
  }

  KASSERT(mouse == mouseTurn);
  KASSERT(slotsLeft > 0);
  KASSERT(!util[bowl-1]);

  util[bowl-1] = true;
  slotsUsed++;
  slotsLeft--;

  if(mouse) {
    numMouseWaiting--;
  }
  else {
    numCatWaiting--;
  }

  lock_release(gLock);  
}

void any_after_eating(unsigned int bowl, bool mouse) {
  KASSERT(gLock != NULL);
  lock_acquire(gLock);

  struct cv *other_condition;
  if(mouse) {
    other_condition = cat_cv;
  }
  else {
    other_condition = mouse_cv;
  }

  KASSERT(mouse == mouseTurn);
  KASSERT(slotsUsed > 0);
  KASSERT(util[bowl-1]);

  util[bowl-1] = false;
  slotsUsed--;

  if(slotsUsed == 0) {
    switch_kind_to(!mouse);
  }

  lock_release(gLock);  
}

/*
 * The CatMouse simulation will call this function each time a cat wants
 * to eat, before it eats.
 * This function should cause the calling thread (a cat simulation thread)
 * to block until it is OK for a cat to eat at the specified bowl.
 *
 * parameter: the number of the bowl at which the cat is trying to eat
 *             legal bowl numbers are 1..NumBowls
 *
 * return value: none
 */

void
cat_before_eating(unsigned int bowl) 
{
  any_before_eating(bowl, false);
}

/*
 * The CatMouse simulation will call this function each time a cat finishes
 * eating.
 *
 * You can use this function to wake up other creatures that may have been
 * waiting to eat until this cat finished.
 *
 * parameter: the number of the bowl at which the cat is finishing eating.
 *             legal bowl numbers are 1..NumBowls
 *
 * return value: none
 */

void
cat_after_eating(unsigned int bowl) 
{
  any_after_eating(bowl, false);
}

/*
 * The CatMouse simulation will call this function each time a mouse wants
 * to eat, before it eats.
 * This function should cause the calling thread (a mouse simulation thread)
 * to block until it is OK for a mouse to eat at the specified bowl.
 *
 * parameter: the number of the bowl at which the mouse is trying to eat
 *             legal bowl numbers are 1..NumBowls
 *
 * return value: none
 */

void
mouse_before_eating(unsigned int bowl) 
{
  any_before_eating(bowl, true);
}

/*
 * The CatMouse simulation will call this function each time a mouse finishes
 * eating.
 *
 * You can use this function to wake up other creatures that may have been
 * waiting to eat until this mouse finished.
 *
 * parameter: the number of the bowl at which the mouse is finishing eating.
 *             legal bowl numbers are 1..NumBowls
 *
 * return value: none
 */

void
mouse_after_eating(unsigned int bowl) 
{
  any_after_eating(bowl, true);
}
