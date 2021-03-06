//
//  mulle_concurrent_hashmap.c
//  mulle-concurrent
//
//  Created by Nat! on 04.03.16.
//  Copyright © 2016 Nat! for Mulle kybernetiK.
//  Copyright © 2016 Codeon GmbH.
//  All rights reserved.
//
//  Redistribution and use in source and binary forms, with or without
//  modification, are permitted provided that the following conditions are met:
//
//  Redistributions of source code must retain the above copyright notice, this
//  list of conditions and the following disclaimer.
//
//  Redistributions in binary form must reproduce the above copyright notice,
//  this list of conditions and the following disclaimer in the documentation
//  and/or other materials provided with the distribution.
//
//  Neither the name of Mulle kybernetiK nor the names of its contributors
//  may be used to endorse or promote products derived from this software
//  without specific prior written permission.
//
//  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
//  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
//  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
//  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
//  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
//  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
//  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
//  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
//  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
//  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
//  POSSIBILITY OF SUCH DAMAGE.
//
#include "mulle_concurrent_hashmap.h"

#include "mulle_concurrent_types.h"
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>


struct _mulle_concurrent_hashvaluepair
{
   intptr_t                 hash;
   mulle_atomic_pointer_t   value;
};


struct _mulle_concurrent_hashmapstorage
{
   mulle_atomic_pointer_t   n_hashs;  // with possibly empty values
   uintptr_t                mask;     // easier to read from debugger if void * size
   
   struct _mulle_concurrent_hashvaluepair  entries[ 1];
};


#define REDIRECT_VALUE     MULLE_CONCURRENT_INVALID_POINTER

#pragma mark -
#pragma mark _mulle_concurrent_hashmapstorage


// n must be a power of 2
static struct _mulle_concurrent_hashmapstorage *
   _mulle_concurrent_alloc_hashmapstorage( unsigned int n,
                                           struct mulle_allocator *allocator)
{
   struct _mulle_concurrent_hashmapstorage  *p;
   
   assert( (~(n - 1) & n) == n);
   
   if( n < 4)
      n = 4;
   
   p = _mulle_allocator_calloc( allocator, 1, sizeof( struct _mulle_concurrent_hashvaluepair) * (n - 1) +
                             sizeof( struct _mulle_concurrent_hashmapstorage));
   
   p->mask = n - 1;
   
   /*
    * in theory, one should be able to use different values for NO_POINTER and
    * INVALID_POINTER
    */
   if( MULLE_CONCURRENT_NO_HASH || MULLE_CONCURRENT_NO_POINTER)
   {
      struct _mulle_concurrent_hashvaluepair   *q;
      struct _mulle_concurrent_hashvaluepair   *sentinel;
      
      q        = p->entries;
      sentinel = &p->entries[ (unsigned int) p->mask];
      while( q <= sentinel)
      {
         q->hash  = MULLE_CONCURRENT_NO_HASH;
         _mulle_atomic_pointer_nonatomic_write( &q->value, MULLE_CONCURRENT_NO_POINTER);
         ++q;
      }
   }
   
   return( p);
}


static unsigned int
   _mulle_concurrent_hashmapstorage_get_max_n_hashs( struct _mulle_concurrent_hashmapstorage *p)
{
   unsigned int   size;
   unsigned int   max;
   
   size = (unsigned int) p->mask + 1;
   max  = size - (size >> 1);
   return( max);
}


static void   *_mulle_concurrent_hashmapstorage_lookup( struct _mulle_concurrent_hashmapstorage *p,
                                                        intptr_t hash)
{
   struct _mulle_concurrent_hashvaluepair   *entry;
   unsigned int                             index;
   unsigned int                             sentinel;
   
   index    = (unsigned int) hash;
   sentinel = index + (unsigned int) p->mask + 1;

   for(;;)
   {
      entry = &p->entries[ index & (unsigned int) p->mask];

      if( entry->hash == MULLE_CONCURRENT_NO_HASH)
         return( MULLE_CONCURRENT_NO_POINTER);
      
      if( entry->hash == hash)
         return( _mulle_atomic_pointer_read( &entry->value));
      
      ++index;
      assert( index != sentinel);  // can't happen we always leave space
   }
}


static struct _mulle_concurrent_hashvaluepair  *
    _mulle_concurrent_hashmapstorage_next_pair( struct _mulle_concurrent_hashmapstorage *p,
                                                unsigned int *index)
{
   struct _mulle_concurrent_hashvaluepair   *entry;
   struct _mulle_concurrent_hashvaluepair   *sentinel;
   
   entry    = &p->entries[ *index];
   sentinel = &p->entries[ (unsigned int) p->mask + 1];

   while( entry < sentinel)
   {
      if( entry->hash == MULLE_CONCURRENT_NO_HASH)
      {
         ++entry;
         continue;
      }
      
      *index = (unsigned int) (entry - p->entries) + 1;
      return( entry);
   }
   return( NULL);
}


//
// insert:
//
//  0      : did insert
//  EEXIST : key already exists (can't replace currently)
//  EBUSY  : this storage can't be written to
//
static int   _mulle_concurrent_hashmapstorage_insert( struct _mulle_concurrent_hashmapstorage *p,
                                                      intptr_t hash,
                                                      void *value)
{
   struct _mulle_concurrent_hashvaluepair   *entry;
   void                               *found;
   unsigned int                       index;
   unsigned int                       sentinel;
   
   assert( hash != MULLE_CONCURRENT_NO_HASH);
   assert( value != MULLE_CONCURRENT_NO_POINTER && value != MULLE_CONCURRENT_INVALID_POINTER);

   index    = (unsigned int) hash;
   sentinel = (unsigned int) (index + (unsigned int) p->mask + 1);
   
   for(;;)
   {
      entry = &p->entries[ index & (unsigned int) p->mask];

      if( entry->hash == MULLE_CONCURRENT_NO_HASH || entry->hash == hash)
      {
         found = __mulle_atomic_pointer_compare_and_swap( &entry->value, value, MULLE_CONCURRENT_NO_POINTER);
         if( found != MULLE_CONCURRENT_NO_POINTER)
         {
            if( found == REDIRECT_VALUE)
               return( EBUSY);
            return( EEXIST);
         }

         if( ! entry->hash)
         {
            _mulle_atomic_pointer_increment( &p->n_hashs);
            entry->hash = hash;
         }
         
         return( 0);
      }
      
      ++index;
      assert( index != sentinel);  // can't happen we always leave space
   }
}


static int   _mulle_concurrent_hashmapstorage_put( struct _mulle_concurrent_hashmapstorage *p,
                                                   intptr_t hash,
                                                   void *value)
{
   struct _mulle_concurrent_hashvaluepair   *entry;
   void                                     *found;
   void                                     *expect;
   unsigned int                             index;
   unsigned int                             sentinel;
   
   assert( value);

   index    = (unsigned int) hash;
   sentinel = (unsigned int) (index + (unsigned int) p->mask + 1);
   
   for(;;)
   {
      entry = &p->entries[ index & (unsigned int) p->mask];

      if( entry->hash == hash)
      {
         expect = MULLE_CONCURRENT_NO_POINTER;
         for(;;)
         {
            found = __mulle_atomic_pointer_compare_and_swap( &entry->value, value, expect);
            if( found == expect)
               return( 0);
            if( found == REDIRECT_VALUE)
               return( EBUSY);
            expect = found;
         }
      }
      
      if( entry->hash == MULLE_CONCURRENT_NO_HASH)
      {
         found = __mulle_atomic_pointer_compare_and_swap( &entry->value, value, MULLE_CONCURRENT_NO_POINTER);
         if( found != MULLE_CONCURRENT_NO_POINTER)
         {
            if( found == REDIRECT_VALUE)
               return( EBUSY);
            return( EEXIST);
         }

         _mulle_atomic_pointer_increment( &p->n_hashs);
         entry->hash = hash;
         
         return( 0);
      }
   
      ++index;
      assert( index != sentinel);  // can't happen we always leave space
   }
}


static int   _mulle_concurrent_hashmapstorage_remove( struct _mulle_concurrent_hashmapstorage *p,
                                                      intptr_t hash,
                                                      void *value)
{
   struct _mulle_concurrent_hashvaluepair   *entry;
   void                                     *found;
   unsigned int                             index;
   unsigned int                             sentinel;
   
   index    = (unsigned int) hash;
   sentinel = index + (unsigned int) p->mask + 1;
   for(;;)
   {
      entry  = &p->entries[ index & (unsigned int) p->mask];

      if( entry->hash == hash)
      {
         found = __mulle_atomic_pointer_compare_and_swap( &entry->value, MULLE_CONCURRENT_NO_POINTER, value);
         if( found == REDIRECT_VALUE)
            return( EBUSY);
         return( found == value ? 0 : ENOENT);
      }
      
      if( entry->hash == MULLE_CONCURRENT_NO_HASH)
         return( ENOENT);
      
      ++index;
      assert( index != sentinel);  // can't happen we always leave space
   }
}


static void   _mulle_concurrent_hashmapstorage_copy( struct _mulle_concurrent_hashmapstorage *dst,
                                                     struct _mulle_concurrent_hashmapstorage *src)
{
   struct _mulle_concurrent_hashvaluepair   *p;
   struct _mulle_concurrent_hashvaluepair   *p_last;
   void                                     *actual;
   void                                     *value;
   
   p      = src->entries;
   p_last = &src->entries[ src->mask];

   for( ;p <= p_last; p++)
   {
      if( ! p->hash)
         continue;

      value = _mulle_atomic_pointer_read( &p->value);
      for(;;)
      {
         if( value == MULLE_CONCURRENT_NO_POINTER)
            break;
         if( value == REDIRECT_VALUE)
            break;
         
         // it's important that we copy over first so
         // No One Gets Left Behind
         _mulle_concurrent_hashmapstorage_put( dst, p->hash, value);
         
         actual = __mulle_atomic_pointer_compare_and_swap( &p->value, REDIRECT_VALUE, value);
         if( actual == value)
            break;
         
         value = actual;
      }
   }
}


#pragma mark -
#pragma mark _mulle_concurrent_hashmap

int  _mulle_concurrent_hashmap_init( struct mulle_concurrent_hashmap *map,
                                     unsigned int size,
                                     struct mulle_allocator *allocator)
{
   struct _mulle_concurrent_hashmapstorage   *storage;
   
   if( ! allocator)
      allocator = &mulle_default_allocator;
   
   assert( allocator->abafree && allocator->abafree != (int (*)()) abort);
   if( ! allocator->abafree || allocator->abafree == (int (*)()) abort)
      return( EINVAL);

   map->allocator = allocator;
   storage        = _mulle_concurrent_alloc_hashmapstorage( size, allocator);

   if( ! storage)
      return( ENOMEM);

   _mulle_atomic_pointer_nonatomic_write( &map->storage.pointer, storage);
   _mulle_atomic_pointer_nonatomic_write( &map->next_storage.pointer, storage);
   
   return( 0);
}


//
// this is called when you know, no other threads are accessing it anymore
//
void  _mulle_concurrent_hashmap_done( struct mulle_concurrent_hashmap *map)
{
   struct _mulle_concurrent_hashmapstorage   *storage;
   struct _mulle_concurrent_hashmapstorage   *next_storage;
   // ABA!

   storage      = _mulle_atomic_pointer_nonatomic_read( &map->storage.pointer);
   next_storage = _mulle_atomic_pointer_nonatomic_read( &map->next_storage.pointer);

   _mulle_allocator_abafree( map->allocator, storage);
   if( storage != next_storage)
      _mulle_allocator_abafree( map->allocator, next_storage);
}


unsigned int  _mulle_concurrent_hashmap_get_size( struct mulle_concurrent_hashmap *map)
{
   struct _mulle_concurrent_hashmapstorage   *p;
   
   p = _mulle_atomic_pointer_read( &map->storage.pointer);
   return( (unsigned int) p->mask + 1);
}


static int  _mulle_concurrent_hashmap_migrate_storage( struct mulle_concurrent_hashmap *map,
                                                       struct _mulle_concurrent_hashmapstorage *p)
{

   struct _mulle_concurrent_hashmapstorage   *q;
   struct _mulle_concurrent_hashmapstorage   *alloced;
   struct _mulle_concurrent_hashmapstorage   *previous;

   assert( p);

   // check if we have a chance to succeed
   alloced = NULL;
   q       = _mulle_atomic_pointer_read( &map->next_storage.pointer);
   if( q == p)
   {
      // acquire new storage
      alloced = _mulle_concurrent_alloc_hashmapstorage( ((unsigned int) p->mask + 1) * 2, map->allocator);
      if( ! alloced)
         return( ENOMEM);
      
      // make this the next world, assume that's still set to 'p' (SIC)
      q = __mulle_atomic_pointer_compare_and_swap( &map->next_storage.pointer, alloced, p);
      if( q != p)
      {
         // someone else produced a next world, use that and get rid of 'alloced'
         _mulle_allocator_abafree( map->allocator, alloced);  // ABA!!
         alloced = NULL;
      }
      else
         q = alloced;
   }
   
   // this thread can partake in copying
   _mulle_concurrent_hashmapstorage_copy( q, p);
   
   // now update world, giving it the same value as 'next_world'
   previous = __mulle_atomic_pointer_compare_and_swap( &map->storage.pointer, q, p);

   // ok, if we succeed free old, if we fail alloced is
   // already gone. this must be an ABA free 
   if( previous == p)
      _mulle_allocator_abafree( map->allocator, previous); // ABA!!
   
   return( 0);
}


void  *_mulle_concurrent_hashmap_lookup( struct mulle_concurrent_hashmap *map,
                                         intptr_t hash)
{
   struct _mulle_concurrent_hashmapstorage   *p;
   void                                      *value;
   
   // won't find invalid hash anyway
retry:
   p     = _mulle_atomic_pointer_read( &map->storage.pointer);
   value = _mulle_concurrent_hashmapstorage_lookup( p, hash);
   if( value == REDIRECT_VALUE)
   {
      if( _mulle_concurrent_hashmap_migrate_storage( map, p))
         return( (void *) MULLE_CONCURRENT_NO_POINTER);
      goto retry;
   }
   return( value);
}


static int   _mulle_concurrent_hashmap_search_next( struct mulle_concurrent_hashmap *map,
                                                    unsigned int  *expect_mask,
                                                    unsigned int  *index,
                                                    intptr_t *p_hash,
                                                    void **p_value)
{
   struct _mulle_concurrent_hashmapstorage   *p;
   struct _mulle_concurrent_hashvaluepair    *entry;
   void                                      *value;
   
retry:
   p = _mulle_atomic_pointer_read( &map->storage.pointer);
   if( *expect_mask && (unsigned int) p->mask != *expect_mask)
      return( ECANCELED);
   
   for(;;)
   {
      entry = _mulle_concurrent_hashmapstorage_next_pair( p, index);
      if( ! entry)
         return( 0);
      
      value = _mulle_atomic_pointer_read( &entry->value);
      if( value == REDIRECT_VALUE)
      {
         if( _mulle_concurrent_hashmap_migrate_storage( map, p))
            return( ENOMEM);
         goto retry;
      }

      if( value != MULLE_CONCURRENT_NO_POINTER)
         break;
   }
   
   if( p_hash)
      *p_hash = entry->hash;
   if( p_value)
      *p_value = value;
   
   if( ! *expect_mask)
      *expect_mask = (unsigned int) p->mask;
   
   return( 1);
}


static inline void   assert_hash_value( intptr_t hash, void *value)
{
   assert( hash != MULLE_CONCURRENT_NO_HASH);
   assert( value != MULLE_CONCURRENT_NO_POINTER);
   assert( value != MULLE_CONCURRENT_INVALID_POINTER);
}


int  _mulle_concurrent_hashmap_insert( struct mulle_concurrent_hashmap *map,
                                       intptr_t hash,
                                       void *value)
{
   struct _mulle_concurrent_hashmapstorage   *p;
   unsigned int                              n;
   unsigned int                              max;

   assert_hash_value( hash, value);
   
retry:
   p = _mulle_atomic_pointer_read( &map->storage.pointer);
   assert( p);

   max = _mulle_concurrent_hashmapstorage_get_max_n_hashs( p);
   n   = (unsigned int) (uintptr_t) _mulle_atomic_pointer_read( &p->n_hashs);
   
   if( n >= max)
   {
      if( _mulle_concurrent_hashmap_migrate_storage( map, p))
         return( ENOMEM);
      goto retry;
   }
   
   switch( _mulle_concurrent_hashmapstorage_insert( p, hash, value))
   {
   case EEXIST :
      return( EEXIST);

   case EBUSY  :
      if( _mulle_concurrent_hashmap_migrate_storage( map, p))
         return( ENOMEM);
      goto retry;
   }

   return( 0);
}


int  mulle_concurrent_hashmap_insert( struct mulle_concurrent_hashmap *map,
                                      intptr_t hash,
                                      void *value)
{
   if( ! map)
      return( EINVAL);
   if( hash == MULLE_CONCURRENT_NO_HASH)
      return( EINVAL);
   if( value == MULLE_CONCURRENT_NO_POINTER || value == MULLE_CONCURRENT_INVALID_POINTER)
      return( EINVAL);

   return( _mulle_concurrent_hashmap_insert( map, hash, value));
}



int  _mulle_concurrent_hashmap_remove( struct mulle_concurrent_hashmap *map,
                                       intptr_t hash,
                                       void *value)
{
   struct _mulle_concurrent_hashmapstorage   *p;
   
   assert_hash_value( hash, value);
   
retry:
   p = _mulle_atomic_pointer_read( &map->storage.pointer);
   switch( _mulle_concurrent_hashmapstorage_remove( p, hash, value))
   {
   case ENOENT :
     return( ENOENT);
         
   case EBUSY  :
      if( _mulle_concurrent_hashmap_migrate_storage( map, p))
         return( ENOMEM);
      goto retry;
   }
   return( 0);
}


int  mulle_concurrent_hashmap_remove( struct mulle_concurrent_hashmap *map,
                                      intptr_t hash,
                                      void *value)
{
   if( ! map)
      return( EINVAL);
   if( hash == MULLE_CONCURRENT_NO_HASH)
      return( EINVAL);
   if( value == MULLE_CONCURRENT_NO_POINTER || value == MULLE_CONCURRENT_INVALID_POINTER)
      return( EINVAL);

   return( _mulle_concurrent_hashmap_remove( map, hash, value));
}


#pragma mark -
#pragma mark not so concurrent enumerator

int  _mulle_concurrent_hashmapenumerator_next( struct mulle_concurrent_hashmapenumerator *rover,
                                               intptr_t *p_hash,
                                               void **p_value)
{
   int        rval;
   void       *value;
   intptr_t   hash;
   
   rval = _mulle_concurrent_hashmap_search_next( rover->map, &rover->mask, &rover->index, &hash, &value);
   
   if( rval != 1)
      return( rval);
   
   if( p_hash)
      *p_hash = hash;
   if( p_value)
      *p_value = value;

   return( 1);
}


#pragma mark -
#pragma mark enumerator based code

//
// obviously just a snapshot at some recent point in time
//
unsigned int  mulle_concurrent_hashmap_count( struct mulle_concurrent_hashmap *map)
{
   unsigned int                                count;
   int                                         rval;
   struct mulle_concurrent_hashmapenumerator   rover;
   
retry:
   count = 0;
   
   rover = mulle_concurrent_hashmap_enumerate( map);
   for(;;)
   {
      rval = _mulle_concurrent_hashmapenumerator_next( &rover, NULL, NULL);
      if( rval == 1)
      {
         ++count;
         continue;
      }

      if( ! rval)
         break;
   
      mulle_concurrent_hashmapenumerator_done( &rover);
      goto retry;
   }
   
   mulle_concurrent_hashmapenumerator_done( &rover);
   return( count);
}


void  *mulle_concurrent_hashmap_lookup_any( struct mulle_concurrent_hashmap *map)
{
   struct mulle_concurrent_hashmapenumerator  rover;
   void  *any;
   
   any   = NULL;
   
   rover = mulle_concurrent_hashmap_enumerate( map);
   _mulle_concurrent_hashmapenumerator_next( &rover, NULL, &any);
   mulle_concurrent_hashmapenumerator_done( &rover);
   
   return( any);
}
