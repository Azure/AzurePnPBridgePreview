/***************************************************************************
 *                                  _   _ ____  _
 *  Project                     ___| | | |  _ \| |
 *                             / __| | | | |_) | |
 *                            | (__| |_| |  _ <| |___
 *                             \___|\___/|_| \_\_____|
 *
 * Copyright (C) 1998 - 2020, Daniel Stenberg, <daniel@haxx.se>, et al.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution. The terms
 * are also available at https://curl.se/docs/copyright.html.
 *
 * You may opt to use, copy, modify, merge, publish, distribute and/or sell
 * copies of the Software, and permit persons to whom the Software is
 * furnished to do so, under the terms of the COPYING file.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ***************************************************************************/ 
/* <DESC>
 * single download with the multi interface's curl_multi_poll
 * </DESC>
 */ 
 
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
 
/* somewhat unix-specific */ 
#include <sys/time.h>
#include <unistd.h>
 
/* curl stuff */ 
#include "curl_wrapper.h"

typedef struct curlPollStatic_ARGS {
  int *bool_continue_ptr;
  int *quit_loop_ptr;
  char testarg[100];
  CURL *static_handle;  
  
} curlPollStatic_ARGS;


// static size_t DataReadCallback(void *contents, size_t size, size_t nmemb, void *userp) {

//     fprintf(stdout,"\n   Size: %d", (int)nmemb);
//     fprintf(stdout,"\n   %s", (char *)contents);
 
//   return nmemb; // realsize;
// };

void *curlPollStatic(void *params)
 {
    // make local copies of input variables
    int *bool_continue = ((struct curlPollStatic_ARGS*)params)->bool_continue_ptr;
    int *quit = ((struct curlPollStatic_ARGS*)params)->quit_loop_ptr;

    fprintf(stdout, "\n**curlPollStatic fn started!**  Test Arg: ");
    fprintf(stdout, "%s", (char*)((struct curlPollStatic_ARGS*)params)->testarg);
    fprintf(stdout, ", quit=%d", *quit);
    fprintf(stdout, ", bool_continue=%d", *bool_continue);

    int key;
    int i = 0;
    while (*quit==0) {
      usleep(5000000);
      i++;      
      
      fprintf(stdout, "\n");
      fprintf(stdout, "curlPollStatic iteration: %d", i);
      fprintf(stdout, ", bool_continue: %d", *bool_continue);
      fprintf(stdout, ", quit: %d", *quit);
      fprintf(stdout, "\n");

      curl_easy_perform(((struct curlPollStatic_ARGS*)params)->static_handle);

      
    }
    *bool_continue=0;
    fprintf(stdout, "\n**Exiting curlPollStatic fn**\n");
 }

int main(void)
{
  CURL *static_handle;
  CURL *stream_handle;
  CURLM *multi_handle;
  int still_running = 1; /* keep number of running handles */ 
  int bool_continue = 1;
  int quit_loop = 0;

  // struct MemoryStruct streamMem;
 
  // streamMem.memory = malloc(10);  /* will be grown as needed by the realloc above */ 
  // streamMem.size = 0;    /* no data at this point */ 
 
  curl_global_init(CURL_GLOBAL_DEFAULT);
 
  static_handle = curl_easy_init();
  stream_handle = curl_easy_init();
 
  curl_easy_setopt(static_handle, CURLOPT_URL, "http://192.168.1.14/api/v1/status");
  curl_easy_setopt(static_handle, CURLOPT_USERPWD, "root:impinj");
  curl_easy_setopt(static_handle, CURLOPT_WRITEFUNCTION, DataReadCallback);
  // curl_easy_setopt(static_handle, CURLOPT_WRITEDATA, (void *)&streamMem);
  
  curl_easy_setopt(stream_handle, CURLOPT_URL, "http://192.168.1.14/api/v1/data/stream");
  curl_easy_setopt(stream_handle, CURLOPT_USERPWD, "root:impinj");
  curl_easy_setopt(stream_handle, CURLOPT_WRITEFUNCTION, DataReadCallback);
  // curl_easy_setopt(stream_handle, CURLOPT_WRITEDATA, (void *)&streamMem);
 
  multi_handle = curl_multi_init();
 
  curl_multi_add_handle(multi_handle, stream_handle);
  // curl_multi_add_handle(multi_handle, static_handle);

  struct curlPollStatic_ARGS *curlPollStatic_args_ptr, curlPollStatic_args;
  curlPollStatic_args.bool_continue_ptr=&bool_continue;
  curlPollStatic_args.quit_loop_ptr=&quit_loop;
  strcpy(curlPollStatic_args.testarg,"test text!");
  curlPollStatic_args.static_handle=static_handle;

  curlPollStatic_args_ptr=&curlPollStatic_args;

  pthread_t tid;
  int rc;
  fprintf(stdout, "\nSpawning thread for key polling...\n");
  rc = pthread_create(&tid, NULL, curlPollStatic, (void *)curlPollStatic_args_ptr);

  while(still_running & bool_continue) {
    CURLMcode mc; /* curl_multi_poll() return code */ 
    int numfds;
    
    /* we start some action by calling perform right away */ 
    mc = curl_multi_perform(multi_handle, &still_running);
 
    if(still_running)
      /* wait for activity, timeout or "nothing" */ 
      mc = curl_multi_poll(multi_handle, NULL, 0, 100, &numfds);
      // fprintf(stdout, "PING\n");
 
    if(mc != CURLM_OK) {
      fprintf(stderr, "curl_multi_wait() failed, code %d.\n", mc);
      break;
    }
  }
 
  rc = pthread_join(tid, NULL);

  curl_multi_remove_handle(multi_handle, stream_handle);
  curl_easy_cleanup(stream_handle);
  curl_easy_cleanup(static_handle);
  curl_multi_cleanup(multi_handle);
  curl_global_cleanup();
 
  return 0;
}