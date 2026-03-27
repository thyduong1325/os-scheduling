#include <iostream>
#include <string>
#include <list>
#include <vector>
#include <chrono>
#include <thread>
#include <mutex>
#include <ncurses.h>
#include "configreader.h"
#include "process.h"

// Shared data for all cores
typedef struct SchedulerData {
    std::mutex queue_mutex;
    ScheduleAlgorithm algorithm;
    uint32_t context_switch;
    uint32_t time_slice;
    std::list<Process*> ready_queue;
    bool all_terminated;
} SchedulerData;

void coreRunProcesses(uint8_t core_id, SchedulerData *data);
void printProcessOutput(std::vector<Process*>& processes);
std::string makeProgressString(double percent, uint32_t width);
uint64_t currentTime();
std::string processStateToString(Process::State state);

int main(int argc, char *argv[])
{
    // Ensure user entered a command line parameter for configuration file name
    if (argc < 2)
    {
        std::cerr << "Error: must specify configuration file" << std::endl;
        exit(EXIT_FAILURE);
    }

    // Declare variables used throughout main
    int i;
    SchedulerData *shared_data = new SchedulerData();
    std::vector<Process*> processes;

    // Read configuration file for scheduling simulation
    SchedulerConfig *config = scr::readConfigFile(argv[1]);

    // Store number of cores in local variable for future access
    uint8_t num_cores = config->cores;

    // Store configuration parameters in shared data object
    shared_data->algorithm = config->algorithm;
    shared_data->context_switch = config->context_switch;
    shared_data->time_slice = config->time_slice;
    shared_data->all_terminated = false;

    // Create processes
    uint64_t start = currentTime();
    for (i = 0; i < config->num_processes; i++)
    {
        Process *p = new Process(config->processes[i], start);
        processes.push_back(p);
        // If process should be launched immediately, add to ready queue
        if (p->getState() == Process::State::Ready)
        {
            shared_data->ready_queue.push_back(p);
        }
    }

    // Free configuration data from memory
    scr::deleteConfig(config);

    // Launch 1 scheduling thread per cpu core
    std::thread *schedule_threads = new std::thread[num_cores];
    for (i = 0; i < num_cores; i++)
    {
        schedule_threads[i] = std::thread(coreRunProcesses, i, shared_data);
    }

    // Main thread work goes here
    initscr();
    int terminated_count = 0;
    uint64_t time_half_terminated = 0;
    while (!(shared_data->all_terminated))
    {
        // Do the following:
        //   - Get current time
        //   - *Check if any processes need to move from NotStarted to Ready (based on elapsed time), and if so put that process in the ready queue
        //   - *Check if any processes have finished their I/O burst, and if so put that process back in the ready queue
        //   - *Check if any running process need to be interrupted (RR time slice expires or newly ready process has higher priority)
        //     - NOTE: ensure processes are inserted into the ready queue at the proper position based on algorithm
        //   - Determine if all processes are in the terminated state
        //   - * = accesses shared data (ready queue), so be sure to use proper synchronization


        // Get current absolute time and elapsed simulation time
        uint64_t current_time = currentTime();
        uint64_t elapsed_time = current_time - start;

        for(int i = 0; i < processes.size(); i++)
        {
            // Move from NotStarted to Ready when start time arrives
            if(processes[i]->getState() == Process::State::NotStarted)
            {
                if(elapsed_time >= processes[i]->getStartTime())
                {
                    processes[i]->setState(Process::State::Ready, currentTime());
                    processes[i]->setBurstStartTime(currentTime()); // Initialize wait timer
                    
                    shared_data->queue_mutex.lock();
                    
                    if(shared_data->algorithm == ScheduleAlgorithm::FCFS)
                    {
                        // FCFS: Place at the back of the queue
                        shared_data->ready_queue.push_back(processes[i]);
                    }
                    else if(shared_data->algorithm == ScheduleAlgorithm::SJF)
                    {
                        // SJF: Insert before the first process with a longer remaining time
                        std::list<Process*>::iterator it = shared_data->ready_queue.begin();
                        while(it != shared_data->ready_queue.end())
                        {
                            if((*it)->getRemainingTime() > processes[i]->getRemainingTime())
                            {
                                shared_data->ready_queue.insert(it, processes[i]);
                                break;
                            }
                            ++it;
                        }
                        if(it == shared_data->ready_queue.end())
                        {
                            shared_data->ready_queue.push_back(processes[i]);
                        }
                    }
                    else if(shared_data->algorithm == ScheduleAlgorithm::PP)
                    {
                        // PP: Insert before the first process with a worse (higher) priority
                        std::list<Process*>::iterator it = shared_data->ready_queue.begin();
                        while(it != shared_data->ready_queue.end())
                        {
                            if((*it)->getPriority() > processes[i]->getPriority())
                            {
                                shared_data->ready_queue.insert(it, processes[i]);
                                break;
                            }
                            ++it;
                        }
                        if(it == shared_data->ready_queue.end())
                        {
                            shared_data->ready_queue.push_back(processes[i]);
                        }
                    }
                    else
                    {
                        // RR: Place at the back of the queue
                        shared_data->ready_queue.push_back(processes[i]);
                    }
                    
                    shared_data->queue_mutex.unlock();
                }
            }

            // Update timers and move finished I/O bursts to the Ready queue
            if(processes[i]->getState() == Process::State::IO)
            {
                processes[i]->updateProcess(current_time);
                
                // If updateProcess finished the I/O, it automatically changes state to Ready
                if(processes[i]->getState() == Process::State::Ready)
                {
                    shared_data->queue_mutex.lock();
                    
                    if(shared_data->algorithm == ScheduleAlgorithm::FCFS) 
                    {
                        shared_data->ready_queue.push_back(processes[i]);
                    }
                    else if(shared_data->algorithm == ScheduleAlgorithm::SJF) 
                    {
                        // SJF: Maintain sorted order based on remaining time
                        std::list<Process*>::iterator it = shared_data->ready_queue.begin();
                        while(it != shared_data->ready_queue.end() && (*it)->getRemainingTime() <= processes[i]->getRemainingTime()) {
                            ++it;
                        }
                        shared_data->ready_queue.insert(it, processes[i]);
                    }
                    else if(shared_data->algorithm == ScheduleAlgorithm::PP) 
                    {
                        // PP: Maintain sorted order based on priority
                        std::list<Process*>::iterator it = shared_data->ready_queue.begin();
                        while(it != shared_data->ready_queue.end() && (*it)->getPriority() <= processes[i]->getPriority()) {
                            ++it;
                        }
                        shared_data->ready_queue.insert(it, processes[i]);
                    }
                    else 
                    {
                        shared_data->ready_queue.push_back(processes[i]);
                    }
                    
                    shared_data->queue_mutex.unlock();
                }
            }

            // Signal lower-priority running processes to interrupt
            if(shared_data->algorithm == ScheduleAlgorithm::PP)
            {
                shared_data->queue_mutex.lock();
                if(!(shared_data->ready_queue.empty()))
                {
                    Process* highest_ready = shared_data->ready_queue.front();
                    for(int j = 0; j < processes.size();j++)
                    {
                        if (processes[j]->getState() == Process::State::Running)
                        {
                            if(processes[j]->getPriority() > highest_ready->getPriority())
                            {
                                processes[j]->interrupt(); // Signal the core thread to handle preemption
                            }
                        }
                    }
                }
                shared_data->queue_mutex.unlock();
            }               
        }

        // Determine if simulation is complete
        terminated_count = 0;
        for(Process* p : processes) {
            if(p->getState() == Process::State::Terminated) terminated_count++;
        }
        
        // Capture the exact elapsed time when the first half of processes terminate
        if (terminated_count >= processes.size() / 2 && time_half_terminated == 0) {
            time_half_terminated = currentTime() - start;
        }
        shared_data->all_terminated = (terminated_count == processes.size());

        // Maybe simply print progress bar for all procs?
        printProcessOutput(processes);

        // sleep 50 ms
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // clear outout
        erase();
    }
    // wait for threads to finish
    for (i = 0; i < num_cores; i++)
    {
        schedule_threads[i].join();
    }

    // print final statistics (use `printw()` for each print, and `refresh()` after all prints)
    uint64_t end = currentTime();
    uint64_t total_time = end - start;
    double total_turnaround_time = 0.0;
    double total_wait_time = 0.0;
    double total_cpu_time = 0.0;
    for (Process* p : processes)
    {
        total_turnaround_time += p->getTurnaroundTime();
        total_wait_time += p->getWaitTime();
        total_cpu_time += p->getCpuTime();
    }

    double average_turnaround_time = total_turnaround_time / processes.size();
    double average_wait_time = total_wait_time / processes.size();

    double cpu_utilization = (total_cpu_time / (num_cores * total_time / 1000.0)) * 100.0; // Convert ms to seconds and calculate percentage
    // Number of processes in first half divided by total time in seconds
    double throughput_first_half = (processes.size() / 2.0) / (time_half_terminated / 1000.0); 
    // Number of processes in second half divided by total time in seconds  
    double throughput_second_half = (processes.size() / 2.0) / ((total_time - time_half_terminated) / 1000.0); 
    // Total number of processes divided by total time in seconds 
    double overall_throughput = processes.size() / (total_time / 1000.0); 
    
    // CPU utilization
    printw("CPU Utilization: %.2f%%\n", cpu_utilization);
    // Average for first half of processes finished
    printw("Throughput (first 50%% of processes): %.2f processes/sec\n", throughput_first_half);
    // Average throughput for second half of processes
    printw("Throughput (second 50%% of processes): %.2f processes/sec\n", throughput_second_half);    //     - Overall average
    // Overall throughput
    printw("Overall Throughput: %.2f processes/sec\n", overall_throughput);    //  - Average turnaround time
    // Average turnaround time
    printw("Average Turnaround Time: %.2f ms\n", average_turnaround_time);    //  - Average waiting time
    // Average wait time
    printw("Average Waiting Time: %.2f ms\n", average_wait_time);
    
    refresh();  

    // Clean up before quitting program
    processes.clear();
    endwin();

    return 0;
}


void coreRunProcesses(uint8_t core_id, SchedulerData *shared_data)
{
    // Work to be done by each core idependent of the other cores
    // Repeat until all processes in terminated state:
    //   - *Get process at front of ready queue
    //   - IF READY QUEUE WAS NOT EMPTY
    //    - Wait context switching load time
    //    - Simulate the processes running (i.e. sleep for short bits, e.g. 5 ms, and call the processes `updateProcess()` method)
    //      until one of the following:
    //      - CPU burst time has elapsed
    //      - Interrupted (RR time slice has elapsed or process preempted by higher priority process)
    //   - Place the process back in the appropriate queue
    //      - I/O queue if CPU burst finished (and process not finished) -- no actual queue, simply set state to IO
    //      - Terminated if CPU burst finished and no more bursts remain -- set state to Terminated
    //      - *Ready queue if interrupted (be sure to modify the CPU burst time to now reflect the remaining time)
    //   - Wait context switching save time
    //  - IF READY QUEUE WAS EMPTY
    //   - Wait short bit (i.e. sleep 5 ms)
    //  - * = accesses shared data (ready queue), so be sure to use proper synchronization

    // Core execution loop: continues until all processes are terminated
    while (!(shared_data->all_terminated))
    {
        // Safely retrieve the next process from the front of the ready queue
        shared_data->queue_mutex.lock();
        if (!(shared_data->ready_queue.empty()))
        {
            Process *p = shared_data->ready_queue.front();
            shared_data->ready_queue.pop_front();
            shared_data->queue_mutex.unlock();

            // Simulate context switch (load penalty)
            std::this_thread::sleep_for(std::chrono::milliseconds(shared_data->context_switch));

            // Update timers to capture the Wait Time spent in the queue
            uint64_t now = currentTime();
            p->updateProcess(now); 

            // Assign to core and initialize active execution state
            p->setState(Process::State::Running, now);
            p->setCpuCore(core_id);
            p->setBurstStartTime(now);

            uint32_t time_on_core = 0; 

            // CPU burst simulation loop: runs until burst finishes or is interrupted
            while (p->getState() == Process::State::Running && !p->isInterrupted()) 
            {
                // Advance simulation by 5ms intervals
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                uint64_t update_now = currentTime();
                time_on_core += update_now - p->getBurstStartTime();
                p->updateProcess(update_now); // Deduct from remaining burst time
                
                // Round Robin: Signal a self-interrupt if the time slice is exceeded
                if (shared_data->algorithm == ScheduleAlgorithm::RR && time_on_core >= shared_data->time_slice) 
                {
                    p->interrupt(); 
                }
            }

            // Post-run cleanup
            p->setCpuCore(-1);

            // Handle preemptions (RR time slice expiration or PP signal from main thread)
            if (p->isInterrupted()) 
            {
                // Change back to Ready and clear the interrupt flag
                p->setState(Process::State::Ready, currentTime());
                p->interruptHandled();
                
                // Safely re-insert the preempted process back into the ready queue
                shared_data->queue_mutex.lock();
                
                if (shared_data->algorithm == ScheduleAlgorithm::SJF) {
                    // SJF: Insert maintaining order of shortest remaining time
                    std::list<Process*>::iterator it = shared_data->ready_queue.begin();
                    while(it != shared_data->ready_queue.end() && (*it)->getRemainingTime() <= p->getRemainingTime()) {
                        ++it;
                    }
                    shared_data->ready_queue.insert(it, p);
                } 
                else if (shared_data->algorithm == ScheduleAlgorithm::PP) {
                    // PP: Insert maintaining order of priority (lowest number first)
                    std::list<Process*>::iterator it = shared_data->ready_queue.begin();
                    while(it != shared_data->ready_queue.end() && (*it)->getPriority() <= p->getPriority()) {
                        ++it;
                    }
                    shared_data->ready_queue.insert(it, p);
                } 
                else {
                    // RR & FCFS: Push directly to the back of the line
                    shared_data->ready_queue.push_back(p);
                }
                
                shared_data->queue_mutex.unlock();
                
                // Restart the wait timer now that it is back in the queue
                p->setBurstStartTime(currentTime()); 
            }

            // Simulate context switch (save penalty) before pulling the next process
            std::this_thread::sleep_for(std::chrono::milliseconds(shared_data->context_switch));
        }
        else
        {
            // Idle core: If the queue is empty, unlock and sleep briefly before checking again
            shared_data->queue_mutex.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
}

void printProcessOutput(std::vector<Process*>& processes)
{
    printw("|   PID | Priority |    State    | Core |               Progress               |\n"); // 36 chars for prog
    printw("+-------+----------+-------------+------+--------------------------------------+\n");
    for (int i = 0; i < processes.size(); i++)
    {
        if (processes[i]->getState() != Process::State::NotStarted)
        {
            uint16_t pid = processes[i]->getPid();
            uint8_t priority = processes[i]->getPriority();
            std::string process_state = processStateToString(processes[i]->getState());
            int8_t core = processes[i]->getCpuCore();
            std::string cpu_core = (core >= 0) ? std::to_string(core) : "--";
            double total_time = processes[i]->getTotalRunTime();
            double completed_time = total_time - processes[i]->getRemainingTime();
            std::string progress = makeProgressString(completed_time / total_time, 36);
            printw("| %5u | %8u | %11s | %4s | %36s |\n", pid, priority,
                   process_state.c_str(), cpu_core.c_str(), progress.c_str());
        }
    }
    refresh();
}

std::string makeProgressString(double percent, uint32_t width)
{
    uint32_t n_chars = percent * width;
    std::string progress_bar(n_chars, '#');
    progress_bar.resize(width, ' ');
    return progress_bar;
}

uint64_t currentTime()
{
    uint64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch()).count();
    return ms;
}

std::string processStateToString(Process::State state)
{
    std::string str;
    switch (state)
    {
        case Process::State::NotStarted:
            str = "not started";
            break;
        case Process::State::Ready:
            str = "ready";
            break;
        case Process::State::Running:
            str = "running";
            break;
        case Process::State::IO:
            str = "i/o";
            break;
        case Process::State::Terminated:
            str = "terminated";
            break;
        default:
            str = "unknown";
            break;
    }
    return str;
}
