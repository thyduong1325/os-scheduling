#include "process.h"

// Process class methods
Process::Process(ProcessDetails details, uint64_t current_time)
{
    int i;
    pid = details.pid;
    start_time = details.start_time;
    num_bursts = details.num_bursts;
    current_burst = 0;
    burst_times = new uint32_t[num_bursts];
    for (i = 0; i < num_bursts; i++)
    {
        burst_times[i] = details.burst_times[i];
    }
    priority = details.priority;
    state = (start_time == 0) ? State::Ready : State::NotStarted;
    if (state == State::Ready)
    {
        launch_time = current_time;
    }
    is_interrupted = false;
    core = -1;
    turn_time = 0;
    wait_time = 0;
    cpu_time = 0;
    total_time = 0;
    for (i = 0; i < num_bursts; i+=2)
    {
        total_time += burst_times[i];
    }
    remain_time = total_time;

    burst_start_time = current_time;
}

Process::~Process()
{
    delete[] burst_times;
}

uint16_t Process::getPid() const
{
    return pid;
}

uint32_t Process::getStartTime() const
{
    return start_time;
}

uint8_t Process::getPriority() const
{
    return priority;
}

uint64_t Process::getBurstStartTime() const
{
    return burst_start_time;
}

Process::State Process::getState() const
{
    return state;
}

bool Process::isInterrupted() const
{
    return is_interrupted;
}

int8_t Process::getCpuCore() const
{
    return core;
}

double Process::getTurnaroundTime() const
{
    return (double)turn_time / 1000.0;
}

double Process::getWaitTime() const
{
    return (double)wait_time / 1000.0;
}

double Process::getCpuTime() const
{
    return (double)cpu_time / 1000.0;
}

double Process::getTotalRunTime() const
{
    // Changed to total_time
    return (double)total_time / 1000.0;
}

double Process::getRemainingTime() const
{
    return (double)remain_time / 1000.0;
}

void Process::setBurstStartTime(uint64_t current_time)
{
    burst_start_time = current_time;
}

void Process::setState(State new_state, uint64_t current_time)
{
    if (state == State::NotStarted && new_state == State::Ready)
    {
        launch_time = current_time;
    }
    state = new_state;
}

void Process::setCpuCore(int8_t core_num)
{
    core = core_num;
}

void Process::interrupt()
{
    is_interrupted = true;
}

void Process::interruptHandled()
{
    is_interrupted = false;
}

void Process::updateProcess(uint64_t current_time)
{
    // Calculate elapsed time since last update and reset tracker
    uint64_t elapsed_time = current_time - burst_start_time;
    burst_start_time = current_time;
    
    // Accumulate turnaround time if process is active in the system
    if (state != State::NotStarted && state != State::Terminated)
    {
        turn_time += elapsed_time;
    }

    // Accumulate wait time if process is waiting in the ready queue
    if (state == State::Ready)
    {
        wait_time += elapsed_time;
    }
    
    // Update execution metrics if process is actively running on CPU
    else if (state == State::Running)
    {
        cpu_time += elapsed_time;
        
        if (burst_times[current_burst] > elapsed_time) {
            burst_times[current_burst] -= elapsed_time;
            remain_time -= elapsed_time;
        } else {
            // Handle CPU burst completion precisely to prevent underflow
            uint64_t actual_elapsed = burst_times[current_burst];
            remain_time -= actual_elapsed;
            burst_times[current_burst] = 0;

            // Terminate if all bursts are done; otherwise, transition to I/O
            if (remain_time == 0) {
                setState(State::Terminated, current_time); 
            } else {
                setState(State::IO, current_time);
                current_burst++; // Advance to the I/O burst index
            }
        }
    }
    
    // Deduct time from current I/O burst if process is in I/O state
    else if (state == State::IO)
    {
        if (burst_times[current_burst] > elapsed_time) {
            burst_times[current_burst] -= elapsed_time;
        } else {
            // Handle I/O completion and return to ready queue
            burst_times[current_burst] = 0;
            setState(State::Ready, current_time);
            
            if (current_burst < num_bursts - 1) {
                current_burst++; // Advance to the next CPU burst index
            }
        }
    }
}

void Process::updateBurstTime(int burst_idx, uint32_t new_time)
{
    burst_times[burst_idx] = new_time;
}
