#include <stdio.h>
#include <stdlib.h>
#include <sys/select.h>
#include <uv.h>
#include <curlbuild.h>
#include <curl.h>
#include "uv_queue.c"

uv_loop_t *loop;
CURLM* curl_handle;
int count = 1;
int waiting_on_flush = 0;
uv_async_t doflush;

//
// Lesson: curl is terrible
//
void notify_for_flush() {
  uv_async_send(&doflush);
}

void flush_queue(uv_async_t* handle, int status) {
  waiting_on_flush = 0;
  curl_multi_perform( curl_handle, &count );
}

size_t read_from_queue(char *ptr, size_t size, size_t nmemb, void *userdata) {
  uv_queue_t* queue = (uv_queue_t*) userdata;
  int to_write = size * nmemb;
  int offset = 0;
  while (to_write - offset > 0) {
    if (!queue->length) {
      break;
    }
    uv_buf_t head = queue->buffers[0];
    if (!head.len) {
      uv_queue_shift(queue, NULL);
      free(head.base);
    }
    else {
      int remaining_in_buf = head.len - to_write;
      int writing;
      if (remaining_in_buf >= 0) {
        writing = to_write - offset;
        memcpy(ptr + offset, head.base, writing);
        uv_buf_t tmp = queue->buffers[0] = uv_buf_init(malloc(remaining_in_buf), remaining_in_buf);
        memcpy(tmp.base, head.base + writing, remaining_in_buf);
        free(head.base);
      }
      else{
        writing = head.len;
        memcpy(ptr + offset, head.base, writing);
        uv_queue_shift(queue, NULL);
        free(head.base);
      }
      offset += writing;
    }
  }
  if (queue->length) {
    notify_for_flush();
  }
  return offset;
}

uv_pipe_t* add_upload(CURLM *curl_handle, const char *url, uv_pipe_t* pipe) {
  CURL *handle = curl_easy_init();
  curl_easy_setopt(handle, CURLOPT_READFUNCTION, read_from_queue);
  curl_easy_setopt(handle, CURLOPT_UPLOAD, 1L);
  curl_easy_setopt(handle, CURLOPT_READDATA, pipe->data);
  curl_easy_setopt(handle, CURLOPT_URL, url);
  return handle;
}

uv_buf_t alloc_buffer(uv_handle_t* stream, size_t suggested_size) {
  return uv_buf_init(malloc(suggested_size), suggested_size);
}
void read_to_curl(uv_stream_t* stream, ssize_t nread, uv_buf_t buf) {
  if (nread <= 0) {
    return;
  }
  uv_buf_t cp = uv_buf_init(buf.base, nread);
  uv_queue_t* queue = stream->data;
  uv_queue_push(queue, cp);
  if(!waiting_on_flush) {
    waiting_on_flush = 1;
    notify_for_flush();
  }
}
void onconnect(uv_stream_t* server, int status) {
  uv_pipe_t* handle = calloc(1, sizeof(uv_pipe_t));
  uv_pipe_init(server->loop, handle, 0);
  handle->data = server->data;
  uv_accept(server, (uv_stream_t*)handle);
  uv_read_start((uv_stream_t*)handle, alloc_buffer, read_to_curl);
}

int main(int argc, char **argv) {
  loop = uv_default_loop();
  
  if (argc <= 2)
    return 0;

  if (curl_global_init(CURL_GLOBAL_ALL)) {
    fprintf(stderr, "Could not init cURL\n");
    return 1;
  }
  
  uv_async_init(loop, &doflush, flush_queue);


  uv_queue_t *queue = (uv_queue_t*) malloc(sizeof(uv_queue_t));
  //printf(__FILE__":%d alloc %p\n",__LINE__,queue);
  if(!queue) exit(1);
  if(uv_queue_init(queue)) exit(11);
  uv_pipe_t *pipe = (uv_pipe_t*) malloc(sizeof(uv_pipe_t));
  //printf(__FILE__":%d alloc %p\n",__LINE__,pipe);
  if(!pipe) exit(3);
  uv_pipe_init(loop, pipe, 0);
  pipe->data = queue;
  curl_handle = curl_multi_init();
  curl_multi_add_handle(curl_handle, add_upload(curl_handle, argv[2], pipe));
  
  uv_pipe_t handle;
  handle.data = queue;
  uv_pipe_init(loop, &handle, 0);
  uv_pipe_bind(&handle, argv[1]);
  uv_listen((uv_stream_t*)&handle, 128, onconnect);
  
  uv_run(loop, UV_RUN_DEFAULT);
  free(queue);
  uv_close((uv_handle_t*)pipe, NULL);
  free(pipe);
  curl_multi_cleanup(curl_handle);
  return 0;
}
