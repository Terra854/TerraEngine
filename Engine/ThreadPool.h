#pragma once
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <memory>
#include <atomic>
#include <future>
#include <iostream>

class ThreadPool;

// Interface for a task
struct ITask {
    std::atomic<int> unresolvedDependencies = -1;
    std::vector<std::shared_ptr<ITask>> dependenciesList;

    virtual ~ITask() = default;

    // Method to clone the task
    virtual std::shared_ptr<ITask> clone() const = 0;
    
    virtual void run() = 0;
    // Use weak_ptr instead of shared_ptr to avoid increasing reference count
    // Otherwise it will not destroy itself on exit, resulting in memory leaks
    virtual void setParentTask(std::weak_ptr<ITask> parent) = 0;
    virtual std::weak_ptr<ITask> getParentTask() = 0;
};

template <typename ReturnType>
struct Task : public ITask, public std::enable_shared_from_this<Task<ReturnType>> {
    std::function<ReturnType()> taskFunc;
    std::promise<ReturnType> taskPromise;
    std::weak_ptr<ITask> parentTask; // Weak pointer to the parent task to avoid increasing reference count

    Task(std::function<ReturnType()> func)
        : ITask(), taskFunc(std::move(func)), parentTask() {}

    std::shared_ptr<ITask> clone() const override {
        //return std::make_shared<Task<ReturnType>>(taskFunc);
        auto clonedTask = std::make_shared<Task<ReturnType>>(taskFunc);

        // Clone each dependency in the dependenciesList
        for (const auto& dependency : this->dependenciesList) {
            auto clonedDependency = dependency->clone();
            clonedTask->dependenciesList.push_back(clonedDependency);
            clonedDependency->setParentTask(clonedTask);
        }

        return clonedTask;
    }

    void run() override {
        try {
            if constexpr (std::is_void_v<ReturnType>) {
                taskFunc();  // Execute the task
                taskPromise.set_value();  // Set value for void type
            }
            else {
                taskPromise.set_value(taskFunc());  // Execute the task and set the value
            }
        }
        catch (...) {
            taskPromise.set_exception(std::current_exception()); // Handle any exceptions
        }
    }

    void setParentTask(std::weak_ptr<ITask> parent) override {
        parentTask = parent;
    }

    std::weak_ptr<ITask> getParentTask() override {
        return parentTask;
    }

    // Add a dependency to this task
    template<typename T>
    void addDependency(const std::shared_ptr<Task<T>>& dependency) {

        std::shared_ptr<ITask> task = dependency->clone();

        dependenciesList.push_back(task);
        task->setParentTask(this->shared_from_this());
    }

    // Delete all dependencies from this task
    void clearDependencies() {
        dependenciesList.clear();
    }

    std::future<ReturnType> getFuture() {
        return taskPromise.get_future();
    }
};

class ThreadPool {
public:
    static ThreadPool& instance();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    void initialize();
    void shutdown();

    // This function needs to be in the header file or else you'll get linker errors
    // Create a task with parameters and return value
    // The task is not enqueued automatically
    // Use enqueue to enqueue the task
    template<class Func, class... Args>
    auto createTask(Func&& func, Args&&... args) -> std::shared_ptr<Task<std::invoke_result_t<Func, Args...>>> {
        using ReturnType = std::invoke_result_t<Func, Args...>;
        auto boundFunc = std::bind(std::forward<Func>(func), std::forward<Args>(args)...);
        auto task = std::make_shared<Task<ReturnType>>(std::function<ReturnType()>(boundFunc));
        return task;
    }

    void enqueue(std::shared_ptr<ITask> weakTask);
    void completeTask(std::shared_ptr<ITask> completedTask);

private:
    ThreadPool() : stop(false) {}
    ~ThreadPool() {}

    void workerThread(int threadNum);

    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;

    std::mutex queue_mutex;
    std::condition_variable condition;
    bool stop;
};