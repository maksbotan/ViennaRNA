#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if VRNA_WITH_PTHREADS
# include <pthread.h>
#endif

#include "ViennaRNA/utils.h"
#include "ViennaRNA/stream_output.h"

#define QUEUE_OVERHEAD  32


struct vrna_ordered_stream_s {
  unsigned int                start;
  unsigned int                end;
  unsigned int                size;
  unsigned int                shift;

  vrna_callback_stream_output *output;
  void                        **data;
  void                        *auxdata;
#if VRNA_WITH_PTHREADS
  pthread_mutex_t             mtx;
#endif
};

struct vrna_ordered_stream_s *
vrna_ostream_init(vrna_callback_stream_output *output,
                  void                        *auxdata)
{
  struct vrna_ordered_stream_s *queue;

  queue = (struct vrna_ordered_stream_s *)vrna_alloc(sizeof(struct vrna_ordered_stream_s));

  queue->start    = 0;
  queue->end      = 0;
  queue->size     = QUEUE_OVERHEAD;
  queue->shift    = 0;
  queue->output   = output;
  queue->auxdata  = auxdata;
  queue->data     = (void **)vrna_alloc(sizeof(void *) * QUEUE_OVERHEAD);

#if VRNA_WITH_PTHREADS
  pthread_mutex_init(&queue->mtx, NULL);
#endif

  return queue;
}


void
vrna_ostream_free(struct vrna_ordered_stream_s *queue)
{
  if (queue) {
    queue->data += queue->shift;
    free(queue->data);
#if VRNA_WITH_PTHREADS
    pthread_mutex_lock(&queue->mtx);
    pthread_mutex_unlock(&queue->mtx);
    pthread_mutex_destroy(&queue->mtx);
#endif
  }
}


void
vrna_ostream_request(struct vrna_ordered_stream_s *queue,
                     unsigned int                 num)
{
  unsigned int i;

  if (queue) {
    if (num >= queue->end) {
#if VRNA_WITH_PTHREADS
      pthread_mutex_lock(&queue->mtx);
#elif defined (_OPENMP)
#   pragma omp critical (vrna_ostream_access)
      {
#endif
      /* check whether we have to increase memory */
      unsigned int new_size = num - queue->shift + 1;

      if (queue->size < new_size) {
        /*
         *  Check whether we can simply move data around to get more space.
         *  We do this only, if more than half of the first buffer is empty
         */
        if ((queue->start - queue->shift > (queue->size / 2)) &&
            ((new_size - queue->start) < queue->size)) {
          /* reset pointer shift */
          queue->data += queue->shift;

          /* move remaining data to the front */
          queue->data = memmove(queue->data,
                                queue->data + queue->start - queue->shift,
                                sizeof(void *) * (queue->end - queue->start + 1));

          /* reset start and pointer shifts */
          queue->shift  = queue->start;
          queue->data   -= queue->start;
        } else {
          /* increase stream buffer size */
          new_size += QUEUE_OVERHEAD;
          /* reset pointer shift */
          queue->data += queue->shift;

          /* reallocate memory blocks */
          queue->data = (void **)vrna_realloc(queue->data, sizeof(void *) * new_size);
          queue->size = new_size;

          /* restore pointer shift */
          queue->data -= queue->shift;
        }
      }

      for (i = queue->end + 1; i <= num; i++)
        queue->data[i] = NULL;

      queue->end = num;
#if VRNA_WITH_PTHREADS
      pthread_mutex_unlock(&queue->mtx);
#elif defined (_OPENMP)
      }
#endif
    }
  }
}


void
vrna_ostream_provide(struct vrna_ordered_stream_s *queue,
                     unsigned int                 i,
                     void                         *data)
{
  unsigned int j;

  if (queue) {
    if ((queue->end < i) || (i < queue->start)) {
      vrna_message_warning(
        "vrna_ostream_provide(): data position (%d) out of range [%d:%d]!",
        i,
        queue->start,
        queue->end);
      return;
    }

    if (data) {
      /* store data */
      queue->data[i] = data;

#if VRNA_WITH_PTHREADS
      pthread_mutex_lock(&queue->mtx);
#elif defined (_OPENMP)
#   pragma omp critical (vrna_ostream_access)
      {
#endif
      if (i == queue->start) {
        /* process all consecutive blocks available from the start */
        for (j = queue->start; (queue->data[j]) && (j <= queue->end); j++) {
          /* process output callback */
          if (queue->output)
            queue->output(queue->auxdata, j, queue->data[j]);

          queue->start++;
        }

        if (queue->end < queue->start)
          queue->end = queue->start;
      }

#if VRNA_WITH_PTHREADS
      pthread_mutex_unlock(&queue->mtx);
#elif defined (_OPENMP)
    }

#endif
    }
  }
}
