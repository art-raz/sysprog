#include "thread_pool.h"

#include <pthread.h>
#include <queue>
#include <vector>
#include <time.h>
#include <errno.h>

struct thread_pool;

struct thread_task {
	thread_task_f function;

	thread_pool* pool = nullptr;

	bool is_finished = false;
	bool is_running = false;
	bool is_awaiting = false;
	bool is_disconnected = false;
	bool was_pushed = false;
	bool is_joined = false;

	mutable pthread_mutex_t mutex;
	pthread_cond_t finished_condition;
};

struct thread_pool {
	std::vector<pthread_t> threads;

	int max_threads = 0;
	int created_threads = 0;
	int active_tasks = 0;
	bool is_stopping = false;

	std::queue<thread_task*> task_queue;

	mutable pthread_mutex_t mutex;
	pthread_cond_t new_task_condition;
};

static void*
thread_worker_function(void* data)
{
	thread_pool* pool = (thread_pool*)data;

	while (true) {
		pthread_mutex_lock(&pool->mutex);

		while (pool->task_queue.empty() && !pool->is_stopping) {
			pthread_cond_wait(&pool->new_task_condition, &pool->mutex);
		}

		if (pool->is_stopping && pool->task_queue.empty()) {
			pthread_mutex_unlock(&pool->mutex);
			return NULL;
		}

		thread_task* task = pool->task_queue.front();
		pool->task_queue.pop();

		pthread_mutex_unlock(&pool->mutex);

		pthread_mutex_lock(&task->mutex);
		task->is_running = true;
		pthread_mutex_unlock(&task->mutex);

		task->function();

		pthread_mutex_lock(&task->mutex);

		task->is_running = false;
		task->is_finished = true;
		task->is_awaiting = false;

		pthread_cond_broadcast(&task->finished_condition);

		pthread_mutex_unlock(&task->mutex);

		pthread_mutex_lock(&pool->mutex);
		pool->active_tasks--;
		pthread_mutex_unlock(&pool->mutex);

		if (task->is_disconnected) {
			pthread_mutex_destroy(&task->mutex);
			pthread_cond_destroy(&task->finished_condition);
			delete task;
		}
	}
}

int
thread_pool_new(int thread_count, struct thread_pool **pool)
{
	if (pool == nullptr)
		return TPOOL_ERR_INVALID_ARGUMENT;

	if (thread_count <= 0 || thread_count > TPOOL_MAX_THREADS)
		return TPOOL_ERR_INVALID_ARGUMENT;

	thread_pool* new_pool = new thread_pool;
	new_pool->max_threads = thread_count;

	pthread_mutex_init(&new_pool->mutex, nullptr);
	pthread_cond_init(&new_pool->new_task_condition, nullptr);

	*pool = new_pool;
	return 0;
}

int
thread_pool_delete(struct thread_pool *pool)
{
	if (pool == nullptr)
		return TPOOL_ERR_INVALID_ARGUMENT;

	pthread_mutex_lock(&pool->mutex);

	if (!pool->task_queue.empty() || pool->active_tasks > 0) {
		pthread_mutex_unlock(&pool->mutex);
		return TPOOL_ERR_HAS_TASKS;
	}

	pool->is_stopping = true;
	pthread_cond_broadcast(&pool->new_task_condition);

	pthread_mutex_unlock(&pool->mutex);

	for (size_t i = 0; i < pool->threads.size(); i++) {
		pthread_join(pool->threads[i], nullptr);
	}

	pthread_mutex_destroy(&pool->mutex);
	pthread_cond_destroy(&pool->new_task_condition);

	delete pool;
	return 0;
}

int
thread_pool_push_task(struct thread_pool *pool, struct thread_task *task)
{
	if (pool == nullptr || task == nullptr)
		return TPOOL_ERR_INVALID_ARGUMENT;

	pthread_mutex_lock(&pool->mutex);

	if (pool->task_queue.size() >= TPOOL_MAX_TASKS) {
		pthread_mutex_unlock(&pool->mutex);
		return TPOOL_ERR_TOO_MANY_TASKS;
	}

	pthread_mutex_lock(&task->mutex);

	task->pool = pool;
	task->is_finished = false;
	task->is_running = false;
	task->is_awaiting = true;
	task->is_disconnected = false;
	task->was_pushed = true;
	task->is_joined = false;

	pthread_mutex_unlock(&task->mutex);

	pool->task_queue.push(task);
	pool->active_tasks++;

	if (pool->created_threads < pool->max_threads &&
	    pool->created_threads < (int)pool->task_queue.size()) {

		pthread_t thread;
		pthread_create(&thread, nullptr, thread_worker_function, pool);

		pool->threads.push_back(thread);
		pool->created_threads++;
	}

	pthread_cond_signal(&pool->new_task_condition);
	pthread_mutex_unlock(&pool->mutex);

	return 0;
}

int
thread_task_new(struct thread_task **task, const thread_task_f &function)
{
	if (task == nullptr)
		return TPOOL_ERR_INVALID_ARGUMENT;

	thread_task* new_task = new thread_task;
	new_task->function = function;

	pthread_mutex_init(&new_task->mutex, nullptr);
	pthread_cond_init(&new_task->finished_condition, nullptr);

	*task = new_task;
	return 0;
}

bool
thread_task_is_finished(const struct thread_task *task)
{
	pthread_mutex_lock(&task->mutex);
	bool result = task->is_finished;
	pthread_mutex_unlock(&task->mutex);
	return result;
}

bool
thread_task_is_running(const struct thread_task *task)
{
	pthread_mutex_lock(&task->mutex);
	bool result = task->is_running;
	pthread_mutex_unlock(&task->mutex);
	return result;
}

int
thread_task_join(struct thread_task *task)
{
	if (task == nullptr || task->pool == nullptr)
		return TPOOL_ERR_TASK_NOT_PUSHED;

	pthread_mutex_lock(&task->mutex);

	while (!task->is_finished) {
		pthread_cond_wait(&task->finished_condition, &task->mutex);
	}

	task->is_joined = true;

	pthread_mutex_unlock(&task->mutex);
	return 0;
}

#if NEED_TIMED_JOIN

int
thread_task_timed_join(struct thread_task *task, double timeout)
{
	if (task == nullptr || task->pool == nullptr)
		return TPOOL_ERR_TASK_NOT_PUSHED;

	pthread_mutex_lock(&task->mutex);

	if (task->is_finished) {
		task->is_joined = true;
		pthread_mutex_unlock(&task->mutex);
		return 0;
	}

	if (timeout <= 0) {
		pthread_mutex_unlock(&task->mutex);
		return TPOOL_ERR_TIMEOUT;
	}

	struct timespec deadline;
	clock_gettime(CLOCK_REALTIME, &deadline);

	time_t sec = (time_t)timeout;
	long nsec = (long)((timeout - sec) * 1e9);

	deadline.tv_sec += sec;
	deadline.tv_nsec += nsec;

	if (deadline.tv_nsec >= 1000000000) {
		deadline.tv_sec++;
		deadline.tv_nsec -= 1000000000;
	}

	while (!task->is_finished) {
		int rc = pthread_cond_timedwait(
			&task->finished_condition,
			&task->mutex,
			&deadline
		);

		if (rc == ETIMEDOUT) {
			pthread_mutex_unlock(&task->mutex);
			return TPOOL_ERR_TIMEOUT;
		}
	}

	task->is_joined = true;

	pthread_mutex_unlock(&task->mutex);
	return 0;
}

#endif

int
thread_task_delete(struct thread_task *task)
{
	if (task == nullptr)
		return TPOOL_ERR_INVALID_ARGUMENT;

	pthread_mutex_lock(&task->mutex);

	if (task->is_awaiting || task->is_running) {
		pthread_mutex_unlock(&task->mutex);
		return TPOOL_ERR_TASK_IN_POOL;
	}

	if (task->was_pushed && !task->is_joined) {
		pthread_mutex_unlock(&task->mutex);
		return TPOOL_ERR_TASK_IN_POOL;
	}

	pthread_mutex_unlock(&task->mutex);

	pthread_mutex_destroy(&task->mutex);
	pthread_cond_destroy(&task->finished_condition);

	delete task;
	return 0;
}

#if NEED_DETACH

int
thread_task_detach(struct thread_task *task)
{
	if (task == nullptr || task->pool == nullptr)
		return TPOOL_ERR_TASK_NOT_PUSHED;

	pthread_mutex_lock(&task->mutex);

	if (task->is_finished) {
		pthread_mutex_unlock(&task->mutex);

		pthread_mutex_destroy(&task->mutex);
		pthread_cond_destroy(&task->finished_condition);
		delete task;

		return 0;
	}

	task->is_disconnected = true;

	pthread_mutex_unlock(&task->mutex);
	return 0;
}

#endif