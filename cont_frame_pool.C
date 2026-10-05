/*
 File: ContFramePool.C

 Author:
 Date  :

 */

/*--------------------------------------------------------------------------*/
/*
 POSSIBLE IMPLEMENTATION
 -----------------------

 The class SimpleFramePool in file "simple_frame_pool.H/C" describes an
 incomplete vanilla implementation of a frame pool that allocates
 *single* frames at a time. Because it does allocate one frame at a time,
 it does not guarantee that a sequence of frames is allocated contiguously.
 This can cause problems.

 The class ContFramePool has the ability to allocate either single frames,
 or sequences of contiguous frames. This affects how we manage the
 free frames. In SimpleFramePool it is sufficient to maintain the free
 frames.
 In ContFramePool we need to maintain free *sequences* of frames.

 This can be done in many ways, ranging from extensions to bitmaps to
 free-lists of frames etc.

 IMPLEMENTATION:

 One simple way to manage sequences of free frames is to add a minor
 extension to the bitmap idea of SimpleFramePool: Instead of maintaining
 whether a frame is FREE or ALLOCATED, which requires one bit per frame,
 we maintain whether the frame is FREE, or ALLOCATED, or HEAD-OF-SEQUENCE.
 The meaning of FREE is the same as in SimpleFramePool.
 If a frame is marked as HEAD-OF-SEQUENCE, this means that it is allocated
 and that it is the first such frame in a sequence of frames. Allocated
 frames that are not first in a sequence are marked as ALLOCATED.

 NOTE: If we use this scheme to allocate only single frames, then all
 frames are marked as either FREE or HEAD-OF-SEQUENCE.

 NOTE: In SimpleFramePool we needed only one bit to store the state of
 each frame. Now we need two bits. In a first implementation you can choose
 to use one char per frame. This will allow you to check for a given status
 without having to do bit manipulations. Once you get this to work,
 revisit the implementation and change it to using two bits. You will get
 an efficiency penalty if you use one char (i.e., 8 bits) per frame when
 two bits do the trick.

 DETAILED IMPLEMENTATION:

 How can we use the HEAD-OF-SEQUENCE state to implement a contiguous
 allocator? Let's look a the individual functions:

 Constructor: Initialize all frames to FREE, except for any frames that you
 need for the management of the frame pool, if any.

 get_frames(_n_frames): Traverse the "bitmap" of states and look for a
 sequence of at least _n_frames entries that are FREE. If you find one,
 mark the first one as HEAD-OF-SEQUENCE and the remaining _n_frames-1 as
 ALLOCATED.

 release_frames(_first_frame_no): Check whether the first frame is marked as
 HEAD-OF-SEQUENCE. If not, something went wrong. If it is, mark it as FREE.
 Traverse the subsequent frames until you reach one that is FREE or
 HEAD-OF-SEQUENCE. Until then, mark the frames that you traverse as FREE.

 mark_inaccessible(_base_frame_no, _n_frames): This is no different than
 get_frames, without having to search for the free sequence. You tell the
 allocator exactly which frame to mark as HEAD-OF-SEQUENCE and how many
 frames after that to mark as ALLOCATED.

 needed_info_frames(_n_frames): This depends on how many bits you need
 to store the state of each frame. If you use a char to represent the state
 of a frame, then you need one info frame for each FRAME_SIZE frames.

 A WORD ABOUT RELEASE_FRAMES():

 When we releae a frame, we only know its frame number. At the time
 of a frame's release, we don't know necessarily which pool it came
 from. Therefore, the function "release_frame" is static, i.e.,
 not associated with a particular frame pool.

 This problem is related to the lack of a so-called "placement delete" in
 C++. For a discussion of this see Stroustrup's FAQ:
 http://www.stroustrup.com/bs_faq2.html#placement-delete

 */
/*--------------------------------------------------------------------------*/

/*--------------------------------------------------------------------------*/
/* DEFINES */
/*--------------------------------------------------------------------------*/

/* -- (none) -- */

/*--------------------------------------------------------------------------*/
/* INCLUDES */
/*--------------------------------------------------------------------------*/

#include "assert.H"
#include "console.H"
#include "cont_frame_pool.H"
#include "utils.H"

/*--------------------------------------------------------------------------*/
/* DATA STRUCTURES */
/*--------------------------------------------------------------------------*/

// head of the list containing all ContFramePools
ContFramePool *ContFramePool::head = nullptr;

/*--------------------------------------------------------------------------*/
/* CONSTANTS */
/*--------------------------------------------------------------------------*/

/* -- (none) -- */

/*--------------------------------------------------------------------------*/
/* FORWARDS */
/*--------------------------------------------------------------------------*/

/* -- (none) -- */

/*--------------------------------------------------------------------------*/
/* METHODS FOR CLASS   C o n t F r a m e P o o l */
/*--------------------------------------------------------------------------*/

ContFramePool::FrameState ContFramePool::get_state(unsigned long _frame_no) {
  unsigned long byte_index = _frame_no / 4;
  unsigned long bit_offset = (_frame_no % 4) * 2;
  unsigned long mask = 0x3 << bit_offset;
  return (FrameState)((this->frame_state[byte_index] & mask) >> bit_offset);
}

void ContFramePool::set_state(unsigned long _frame_no,
                              ContFramePool::FrameState _state) {
  unsigned long byte_index = _frame_no / 4;
  unsigned long bit_offset = (_frame_no % 4) * 2;
  unsigned long mask = 0x3 << bit_offset;
  this->frame_state[byte_index] = (this->frame_state[byte_index] & ~mask) |
                                  (((unsigned char)_state) << bit_offset);
}

void ContFramePool::commit_frames(unsigned long rel_base, unsigned long n) {
  // ensure entire block of frames is free
  assert(this->get_state(rel_base) == ContFramePool::FrameState::Free);
  for (unsigned long i = 1; i < n; i++) {
    assert(this->get_state(rel_base + i) == ContFramePool::FrameState::Free);
  }

  // mark contiguous block of frames as used
  // first frame in block should be HoS
  set_state(rel_base, ContFramePool::FrameState::HoS);
  for (unsigned long i = 1; i < n; i++) {
    set_state(rel_base + i, ContFramePool::FrameState::Used);
  }

  // update free frames counter
  this->n_free_frames -= n;
}

void ContFramePool::free_frames(unsigned long rel_base) {
  // ensure the first frame is actually the first frame of a contiguous
  // block of frames
  assert(this->get_state(rel_base) == ContFramePool::FrameState::HoS);

  // free header frame
  this->set_state(rel_base, ContFramePool::FrameState::Free);

  // keep track of the number of freed frames to update pool state later
  unsigned long no_freed_frames = 1;

  // free remaining frames
  for (unsigned long fno = rel_base + 1;
       fno < this->n_frames &&
       this->get_state(fno) == ContFramePool::FrameState::Used;
       fno++) {
    this->set_state(fno, ContFramePool::FrameState::Free);
    no_freed_frames++;
  }

  // update pool's free frames counter
  this->n_free_frames += no_freed_frames;
}

ContFramePool::ContFramePool(unsigned long _base_frame_no,
                             unsigned long _n_frames,
                             unsigned long _info_frame_no) {

  // zero length frame pool is nonsensical
  assert(_n_frames > 0);

  // number of frames needed to hold frame states (assuming frame is self
  // hosted)
  unsigned long n_needed_info_frame = needed_info_frames(_n_frames);

  // frame pool needs enough frames to hold info if self hosted
  if (_info_frame_no == 0) {
    assert(_n_frames >= n_needed_info_frame);
  }

  // initialize internals
  this->base_frame_no = _base_frame_no;
  this->n_frames = _n_frames;
  this->info_frame_no = _info_frame_no;
  this->n_free_frames = _n_frames;

  // point frame_state at correct frame address
  if (_info_frame_no == 0) {
    this->frame_state = (unsigned char *)(base_frame_no * FRAME_SIZE);
  } else {
    this->frame_state = (unsigned char *)(info_frame_no * FRAME_SIZE);
  }

  // set allocatable frames as free
  for (unsigned long fno = 0; fno < _n_frames; fno++) {
    set_state(fno, ContFramePool::FrameState::Free);
  }

  // reserve info frames if self hosted
  if (_info_frame_no == 0) {
    this->commit_frames(0, n_needed_info_frame);
  }

  this->next = head;
  head = this;
}

// ! Note that this function halts the kernel entirely whenever it fails to find a contiguous block of frames of the requested size and assets that there is at least enough frames to store the requested amount.
unsigned long ContFramePool::get_frames(unsigned int _n_frames) {
  // ensure the pool can at least fit the number of requested frames
  // note that this does NOT guarantee that the block is available, just that
  // the pool can fit the requested number of frames
  assert(this->n_free_frames >= _n_frames);

  // keep track of contiguous block size since last HoS
  unsigned long block_start = 0;
  unsigned long block_size = 0;

  // search for contiguous block of frames that can fit the requested amount
  for (unsigned long i = 0; i < this->n_frames; i++) {
    if (this->get_state(i) == ContFramePool::FrameState::Free) {
      if (block_size == 0) {
        block_start = i;
      }
      block_size++;
    } else {
      block_size = 0;
    }

    // the current block can fit the requested amount of frames
    // mark the block as used and return a reference to the start of it
    if (block_size >= _n_frames) {
      this->commit_frames(block_start, _n_frames);
      return this->base_frame_no + block_start;
    }
  }

  // contiguous block of frames of the request size does not exist on this pool
  assert(false);
  return 0;
}

void ContFramePool::mark_inaccessible(unsigned long _base_frame_no,
                                      unsigned long _n_frames) {

  // ensure block of frames is withing the frame pool
  assert(_base_frame_no >= this->base_frame_no &&
         _base_frame_no + _n_frames <= this->base_frame_no + this->n_frames);

  // convert _base_frame_no to relative frame number then mark block as used
  this->commit_frames(_base_frame_no - this->base_frame_no, _n_frames);
}

void ContFramePool::release_frames(unsigned long _first_frame_no) {
  // iterate the ContFramePool list
  for (ContFramePool *pool = head; pool != nullptr; pool = pool->next) {
    // check if target frame is within the current pool's contiguous block of
    // frames
    if (_first_frame_no >= pool->base_frame_no &&
        _first_frame_no < pool->base_frame_no + pool->n_frames) {

      // pool state getter and setter use relative frame numbers
      unsigned long rel = _first_frame_no - pool->base_frame_no;

      // free targeted block of frames from pool
      pool->free_frames(rel);

      return;
    }
  }
  assert(false); // target frame does not belong to any ContFramePool
}

unsigned long ContFramePool::needed_info_frames(unsigned long _n_frames) {
  return _n_frames / (FRAME_SIZE * 4) +
         ((_n_frames % (FRAME_SIZE * 4) > 0) ? 1 : 0);
}
