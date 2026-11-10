#include "check.hpp"
#include "concurrency/ThreadPool.hpp"
#include <unordered_map>
#include <iostream>
#include <string>
#include <chrono>
#include <atomic>

//testing threadpool

//stress test
void run_stress_test() {
    std::cout << "\n[TEST 1] Firing 1000 tasks into the pool...\n";
    std::atomic<int> completed_tasks{0};
    const int total_tasks = 1000;

    //map tracks how many tasks each specific OS thread completes
    std::unordered_map<std::thread::id, int> thread_workload;
    std::mutex map_mutex; //protects the map from race conditions
    {
        ThreadPool pool(4); 

        for (int i = 0; i < total_tasks; ++i) {
            pool.enqueue([&completed_tasks, &thread_workload, &map_mutex]() {

                // simulate physical I/O delay
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                completed_tasks++;

                // tally the work under a quick lock
                std::thread::id this_id = std::this_thread::get_id();
                {
                    std::lock_guard<std::mutex> lock(map_mutex);
                    thread_workload[this_id]++;
                }
            });
        }
    } // pool goes out of scope here, triggering the destructor and waiting for tasks to finish

    CHECK_EQ(completed_tasks.load(), total_tasks);
    if (completed_tasks == total_tasks) {
        std::cout << "[SUCCESS] ThreadPool safely processed all " << completed_tasks << " tasks.\n";
        // final workload distribution map
        std::cout << "   -> Workload Distribution Breakdown:\n";
        for (const auto& pair : thread_workload) {
            std::cout << "      [Worker " << pair.first << "] completed " 
                      << pair.second << " tasks.\n";
        }
    } else {
        std::cerr << "[FAILED] Only finished " << completed_tasks << " tasks.\n";
    }
}
//termination test
void run_terminate_test() {
    std::cout << "\n[TEST 2] Executing the 'NO' (Graceful Terminate) Case...\n";
    auto start_time = std::chrono::steady_clock::now();

    {
        ThreadPool pool(2);
        
        pool.enqueue([] {
            // print unique ID of the worker thread that picked up this task
            std::cout << "   -> [Worker " << std::this_thread::get_id() << "] Started a 2-second task...\n";
            std::this_thread::sleep_for(std::chrono::seconds(2));
            std::cout << "   -> [Worker " << std::this_thread::get_id() << "] Finished the 2-second task!\n";
        });

        // print unique ID of the main application thread
        std::cout << "   -> [Main Thread " << std::this_thread::get_id() << "] Attempting to destroy the pool RIGHT NOW...\n";
        
    } // pool destructor is called here, physically freezing the main thread

    auto end_time = std::chrono::steady_clock::now();
    std::chrono::duration<double> elapsed = end_time - start_time;

    std::cout << "   -> [Main Thread " << std::this_thread::get_id() << "] Pool successfully destroyed. Time: " 
              << elapsed.count() << " seconds.\n";

    CHECK(elapsed.count() >= 2.0);
    if (elapsed.count() >= 2.0) std::cout << "[SUCCESS] The destructor successfully blocked the main thread until the worker finished!\n";
    else {
        std::cerr << "[FAILED] The pool destroyed itself violently!\n";
    }
}

int main(int argc, char* argv[]) {
    std::cout << "==========================================\n";
    std::cout << "   VORTEX ENGINE: CONCURRENCY TEST SUITE  \n";
    std::cout << "==========================================\n";

    // if no arguments were passed, show the help menu
    if (argc == 1) {
        std::cout << "\n[ERROR] No test specified.\n";
        std::cout << "Usage: ./build/test_pool [ stress | terminate | all ]\n\n";
        return 1; 
    }

    std::string test_target = argv[1];

    if (test_target == "stress") run_stress_test();
    else if (test_target == "terminate") run_terminate_test();
    else if (test_target == "all") {
        run_stress_test();
        std::cout << "\n------------------------------------------\n";
        run_terminate_test();
    } 
    else{
        std::cout << "\n[ERROR] Unknown command: " << test_target << "\n";
        std::cout << "Available commands: stress, terminate, all\n\n";
        return 1;
    }
    return finish_tests("test_pool");
}