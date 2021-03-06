/**
 * This file is part of stc-rpc.
 *
 * Copyright (C) 2012 Alexander Tarasikov <alexander.tarasikov@gmail.com>
 *
 * stc-rpc is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * stc-rpc is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with stc-rpc.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>

#include <pthread.h>

#include <stc_rpc.h>
#include <stc_log.h>

#define CtrlPipe_Shutdown 0

struct rpc {
	int fd;
	int pipefd[2];
	int pipectrl[2]; // 用来关闭线程的管道
	int active;
	rpc_handler_t handler;
	
	pthread_mutex_t fd_mutex;
	pthread_t rpc_thread;

	pthread_mutex_t cond_mtx;
	pthread_cond_t cond;

	pthread_mutex_t pipe_mtx;
};

enum {
	READ_END = 0,
	WRITE_END = 1,
};

static void set_nonblocking(int fd) {
	int flags = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static inline int max(int a, int b) {
	return a > b ? a : b;
}

static void rpc_cond_wait(struct rpc *rpc) {
	pthread_mutex_lock(&rpc->cond_mtx);
	pthread_cond_wait(&rpc->cond, &rpc->cond_mtx);
	pthread_mutex_unlock(&rpc->cond_mtx);
}

static void rpc_cond_signal(struct rpc *rpc) {
	pthread_mutex_lock(&rpc->cond_mtx);
	pthread_cond_broadcast(&rpc->cond);
	pthread_mutex_unlock(&rpc->cond_mtx);
}

static int select_write(int fd) {
	fd_set fds;
	FD_ZERO(&fds);
	FD_SET(fd, &fds);
	
	struct timeval tv = {
		.tv_usec = RPC_TIMEOUT_US,
	};

	return select(fd + 1, NULL, &fds, NULL, &tv);
}

static int select_read(int fd) {
	fd_set fds;
	FD_ZERO(&fds);
	FD_SET(fd, &fds);

	struct timeval tv = {
			.tv_usec = RPC_TIMEOUT_US,
	};

	return select(fd + 1, &fds, NULL, NULL, &tv);
}

static int rpc_read(int fd, void *data, size_t size) {
	int ret;
	LOG_ENTRY;
	do {
		ret = recv(fd, data, size, 0);
	} while (ret < 0 && errno == EAGAIN);
	if (ret < 0) {
		RPC_PERROR("recv");
	}
	RPC_DEBUG("rpc_read bytes: %d\n", ret);
	LOG_EXIT;
	return ret;
}

static int rpc_write(int fd, void *data, size_t size) {
	int ret;
	LOG_ENTRY;
	do {
		select_write(fd);
		ret = send(fd, data, size, 0);
	} while (ret < 0 && errno == EAGAIN);
	if (ret < 0) {
		RPC_PERROR("send");
	}
	LOG_EXIT;
	return ret;
}

static int rpc_send(struct rpc *rpc) {
	LOG_ENTRY;

	struct rpc_request_t req;

	pthread_mutex_lock(&rpc->pipe_mtx);
	int rc = read(rpc->pipefd[READ_END], &req, sizeof(req));
	pthread_mutex_unlock(&rpc->pipe_mtx);

	if (rc < 0) {
		RPC_PERROR("read");
		goto fail;
	}

	RPC_DEBUG(">>> code %d", req.header.code);
	
	if (rpc_write(rpc->fd, &req.header, sizeof(rpc_request_hdr_t)) < 0) {
		RPC_PERROR("rpc_write");
		goto fail;
	}
	RPC_DEBUG(">>> sent header");

	if (rpc_read(rpc->fd, &req.reply, sizeof(rpc_reply_t)) < 0) {
		RPC_PERROR("rpc_read");
		goto fail;
	}
	RPC_DEBUG(">>> reply code %d", req.reply.code);
	//// 添加回复消息的处理
	if (rpc->handler(&req.header, &req.reply)) {
		RPC_ERROR("failed to handle message");
		goto fail;
	}
	RPC_DEBUG("<<< handled reply message %d", req.reply.code);
	//// 添加回复消息的处理

	if (req.reply_marker) {
		req.reply_marker[0] = 1;
	}
	rpc_cond_signal(rpc);

	LOG_EXIT;
	return 0;
fail:
	return -1;
}

static int rpc_recv(struct rpc *rpc) {
	struct rpc_request_hdr_t hdr;
	struct rpc_reply_t reply;
	LOG_ENTRY;
		
	memset(&reply, 0, sizeof(reply));
	int ret = rpc_read(rpc->fd, &hdr, sizeof(hdr));
	if (ret <= 0) {
		if (ret == 0) {
			RPC_DEBUG("The other end of the socket may have been disconnected.");
		} else {
			RPC_PERROR("rpc_read");
		}
		goto fail;
	}

	RPC_DEBUG("<<< header code %d", hdr.code);
	if (rpc->handler(&hdr, &reply)) {
		RPC_ERROR("failed to handle message");
		goto fail;
	}
	RPC_DEBUG("<<< handled message %d", hdr.code);

	if (rpc_write(rpc->fd, &reply, sizeof(reply)) < 0) {
		RPC_PERROR("rpc_write");
		goto fail;
	}
	RPC_DEBUG("<<< done with message %d", hdr.code);

	LOG_EXIT;
	return 0;
fail:
	return -1;
}

static void* do_rpc_thread(void *data) {
	LOG_ENTRY;

	struct rpc *rpc = data;
	if (!rpc || !rpc->handler) {
		RPC_PUTS("bad rpc");
		LOG_EXIT;
		return NULL;
	}
	
	rpc->active = 1;
	rpc_cond_signal(rpc);

	fd_set fds;

	while (rpc->active) {
		RPC_DEBUG("+loop");
		
		FD_ZERO(&fds);
		FD_SET(rpc->fd, &fds);
		FD_SET(rpc->pipefd[READ_END], &fds);
		FD_SET(rpc->pipectrl[READ_END], &fds);

		select(max(max(rpc->fd, rpc->pipefd[READ_END]), rpc->pipectrl[READ_END]) + 1, &fds, NULL, NULL, NULL);
		pthread_mutex_lock(&rpc->fd_mutex);
		if (FD_ISSET(rpc->fd, &fds)) {
			RPC_DEBUG("receiving RPC message");
			if (rpc_recv(rpc) < 0) {
				RPC_ERROR("receive error");
				rpc->active = 0;
				goto handled;
			}
		}
		if (FD_ISSET(rpc->pipefd[READ_END], &fds)) {
			RPC_DEBUG("calling remote party");
			if (rpc_send(rpc) < 0) {
				RPC_ERROR("call error");
				rpc->active = 0;
				goto handled;
			}
		}
		if (FD_ISSET(rpc->pipectrl[READ_END], &fds)) {
			RPC_DEBUG("Control command received");
			char c = CtrlPipe_Shutdown;
			TEMP_FAILURE_RETRY(read(rpc->pipectrl[READ_END], &c, 1));
			if (c == CtrlPipe_Shutdown) {
				RPC_DEBUG("Shutting down the thread now!");
				rpc->active = 0;
				goto handled;
			}
		}
handled:
		pthread_mutex_unlock(&rpc->fd_mutex);
		RPC_DEBUG("-loop");
	}
	rpc_cond_signal(rpc);

	LOG_EXIT;
	return NULL;
}

int __rpc_call(struct rpc *rpc, struct rpc_request_t *req, int wait) {
	int ret = 0;
	LOG_ENTRY;

	int done = 0;
	req->reply_marker = &done;

	RPC_DEBUG("%s: writing to pipe code=%d", __func__, req->header.code);

	pthread_mutex_lock(&rpc->pipe_mtx);
	ret = write(rpc->pipefd[WRITE_END], req, sizeof(rpc_request_t));
	pthread_mutex_unlock(&rpc->pipe_mtx);

	if (ret < 0) {
		RPC_PERROR("write");
		goto fail;
	}

	if (!wait) {
		goto done;
	}

	while (!done && rpc->active) {
		RPC_DEBUG("%s: waiting for reply", __func__);
		rpc_cond_wait(rpc);
	}
	RPC_DEBUG("got reply");

done:
	ret = 0;

	if (!done || !rpc->active) {
		RPC_DEBUG("error");
		ret = -1;
	}

fail:
	LOG_EXIT;
	return ret;
}

int rpc_call(struct rpc *rpc, struct rpc_request_t *req) {
	LOG_ENTRY;
	int rc = __rpc_call(rpc, req, 1);
	LOG_EXIT;
	return rc;
}

int rpc_call_noreply(struct rpc *rpc, struct rpc_request_t *req) {
	LOG_ENTRY;
	int rc = __rpc_call(rpc, req, 0);
	LOG_EXIT;
	return rc;
}

rpc_t *rpc_alloc(void) {
	rpc_t *ret = NULL;

	ret = (rpc_t*)malloc(sizeof(rpc_t));
	if (!ret) {
		RPC_DEBUG("out of memory");
		goto fail;
	}
	memset(ret, 0, sizeof(rpc_t));

	return ret;
fail:
	if (ret) {
		free(ret);
	}
	return NULL;
}

void rpc_free(rpc_t *rpc) {
	if (!rpc) {
		return;
	}
	free(rpc);
}

int rpc_init(int fd, rpc_handler_t handler, rpc_t *rpc) {
	if (fd < 0) {
		RPC_DEBUG("bad fd %d", fd);
		goto fail;
	}

	if (!handler) {
		RPC_DEBUG("handler is NULL");
		goto fail;
	}
	
	if (!rpc) {
		RPC_DEBUG("rpc is NULL");
		goto fail;
	}

	if (pthread_mutex_init(&rpc->fd_mutex, NULL)) {
		RPC_PERROR("init fd mutex");
		goto fail;
	}

	if (pthread_mutex_init(&rpc->cond_mtx, NULL)) {
		RPC_PERROR("init condition mutex");
		goto fail_cond_mutex;
	}

	if (pthread_cond_init(&rpc->cond, NULL)) {
		RPC_PERROR("init condition");
		goto fail_cond;
	}

	if (pthread_mutex_init(&rpc->pipe_mtx, NULL)) {
		RPC_ERROR("init pipe mutex");
		goto fail_pipe_mutex;
	}

	if (pipe(rpc->pipefd) < 0) {
		RPC_PERROR("pipe");
		goto fail_pipe;
	}

	if (pipe(rpc->pipectrl) < 0) {
		RPC_PERROR("pipe ctrl");
		goto fail_pipe;
	}

	set_nonblocking(rpc->pipefd[WRITE_END]);
	set_nonblocking(rpc->pipefd[READ_END]);
	set_nonblocking(rpc->pipectrl[WRITE_END]);
	set_nonblocking(rpc->pipectrl[READ_END]);
	set_nonblocking(fd);
	
	rpc->fd = fd;
	rpc->handler = handler;

	return 0;

fail_pipe:
	pthread_mutex_destroy(&rpc->pipe_mtx);
fail_pipe_mutex:
	pthread_cond_destroy(&rpc->cond);
fail_cond:
	pthread_mutex_destroy(&rpc->cond_mtx);
fail_cond_mutex:
	pthread_mutex_destroy(&rpc->fd_mutex);
fail:
	return -1;
}

int rpc_start(rpc_t *rpc) {
	if (!rpc) {
		RPC_DEBUG("rpc is NULL");
		goto fail;
	}

	if (!rpc->handler) {
		RPC_DEBUG("rpc handler is NULL");
		goto fail;
	}
	
	if (pthread_create(&rpc->rpc_thread, NULL, do_rpc_thread, rpc)) {
		RPC_PERROR("pthread_create");
		goto fail;
	}

	while (!rpc->active) {
		rpc_cond_wait(rpc);
	}

	return 0;

fail:
	return -1;
}

int rpc_join(rpc_t *rpc) {
	if (!rpc) {
		return 0;
	}
	return pthread_join(rpc->rpc_thread, NULL);
}

int rpc_stop(rpc_t *rpc) {
	if (!rpc) {
		return 0;
	}
	rpc->active = 0;

	rpc_cond_signal(rpc);

	// SIGKILL会导致整个进程崩溃
//	int rc = pthread_kill(rpc->rpc_thread, SIGKILL);
	char c = CtrlPipe_Shutdown;
	TEMP_FAILURE_RETRY(write(rpc->pipectrl[WRITE_END], &c, 1));

	int rc = rpc_join(rpc);

	close(rpc->pipefd[READ_END]);
	close(rpc->pipefd[WRITE_END]);
	close(rpc->pipectrl[READ_END]);
	close(rpc->pipectrl[WRITE_END]);
	return rc;
}
