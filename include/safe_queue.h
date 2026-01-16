/*
 * ......................................&&.........................
 * ....................................&&&..........................
 * .................................&&&&............................
 * ...............................&&&&..............................
 * .............................&&&&&&..............................
 * ...........................&&&&&&....&&&..&&&&&&&&&&&&&&&........
 * ..................&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&..............
 * ................&...&&&&&&&&&&&&&&&&&&&&&&&&&&&&.................
 * .......................&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&.........
 * ...................&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&...............
 * ..................&&&   &&&&&&&&&&&&&&&&&&&&&&&&&&&&&............
 * ...............&&&&&@  &&&&&&&&&&..&&&&&&&&&&&&&&&&&&&...........
 * ..............&&&&&&&&&&&&&&&.&&....&&&&&&&&&&&&&..&&&&&.........
 * ..........&&&&&&&&&&&&&&&&&&...&.....&&&&&&&&&&&&&...&&&&........
 * ........&&&&&&&&&&&&&&&&&&&.........&&&&&&&&&&&&&&&....&&&.......
 * .......&&&&&&&&.....................&&&&&&&&&&&&&&&&.....&&......
 * ........&&&&&.....................&&&&&&&&&&&&&&&&&&.............
 * ..........&...................&&&&&&&&&&&&&&&&&&&&&&&............
 * ................&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&............
 * ..................&&&&&&&&&&&&&&&&&&&&&&&&&&&&..&&&&&............
 * ..............&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&....&&&&&............
 * ...........&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&......&&&&............
 * .........&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&.........&&&&............
 * .......&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&...........&&&&............
 * ......&&&&&&&&&&&&&&&&&&&...&&&&&&...............&&&.............
 * .....&&&&&&&&&&&&&&&&............................&&..............
 * ....&&&&&&&&&&&&&&&.................&&...........................
 * ...&&&&&&&&&&&&&&&.....................&&&&......................
 * ...&&&&&&&&&&.&&&........................&&&&&...................
 * ..&&&&&&&&&&&..&&..........................&&&&&&&...............
 * ..&&&&&&&&&&&&...&............&&&.....&&&&...&&&&&&&.............
 * ..&&&&&&&&&&&&&.................&&&.....&&&&&&&&&&&&&&...........
 * ..&&&&&&&&&&&&&&&&..............&&&&&&&&&&&&&&&&&&&&&&&&.........
 * ..&&.&&&&&&&&&&&&&&&&&.........&&&&&&&&&&&&&&&&&&&&&&&&&&&.......
 * ...&&..&&&&&&&&&&&&.........&&&&&&&&&&&&&&&&...&&&&&&&&&&&&......
 * ....&..&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&...........&&&&&&&&.....
 * .......&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&..............&&&&&&&....
 * .......&&&&&.&&&&&&&&&&&&&&&&&&..&&&&&&&&...&..........&&&&&&....
 * ........&&&.....&&&&&&&&&&&&&.....&&&&&&&&&&...........&..&&&&...
 * .......&&&........&&&.&&&&&&&&&.....&&&&&.................&&&&...
 * .......&&&...............&&&&&&&.......&&&&&&&&............&&&...
 * ........&&...................&&&&&&.........................&&&..
 * .........&.....................&&&&........................&&....
 * ...............................&&&.......................&&......
 * ................................&&......................&&.......
 * .................................&&..............................
 * ..................................&..............................
 *
 * @Author: wtwei
 * @Date: 2023-07-31 15:12:53
 * @LastEditTime: 2023-08-18 13:59:41
 * @Description:
 *
 */

#ifndef MESSAGE_QUEUE_H
#define MESSAGE_QUEUE_H

#include <cassert>
#include <future>
#include <mutex>
#include <queue>
template <typename T> class Queue {
  public:
    // Constructor
    Queue(){};
    void setLen(int max_size)
    {
        maxSize = max_size;
    }
    // Method to send msg to the message queue using move semantics
    int push(T&& msg, int timeout_ms = -1)
    {
        // Create a lock guard to protect the queue from data race
        std::unique_lock<std::mutex> lock(_mtx);
        if (timeout_ms < 0)
            cvPush.wait(lock, [this]() { return _queue.size() < maxSize; });
        else {
            bool status =
                cvPush.wait_for(lock, chrono::milliseconds(timeout_ms),
                                [this]() { return _queue.size() < maxSize; });
            if (!status)
                return -1;
        }
        // Push the msg to the back of the queue
        _queue.push(std::move(msg));

        // Increment the size of the queue
        _size++;

        // notify that the element has been added in the queue
        cvPop.notify_one();
        return 0;
    }
    /*void push(T &msg) {

    }*/
    // Method to receive msg from the queue using move semantics

    int get(T& msg, int timeout_ms = -1)
    {
        // Create a unique lock to pass it in the wait method of conditional
        // variabe
        std::unique_lock<std::mutex> uLock(_mtx);

        // Check the conditon under lock and than enter the wait based on
        // condition
        if (timeout_ms < 0)
            cvPop.wait(uLock, [this] { return !_queue.empty(); });
        else {
            bool status =
                cvPop.wait_for(uLock, chrono::milliseconds(timeout_ms),
                               [this] { return !_queue.empty(); });
            if (!status)  // 超时
                return -1;
        }

        assert(!_queue.empty());
        // Move the front element from the queue
        msg = std::move(_queue.front());

        // Remove the front element from the queue
        _queue.pop();

        // Increment the size of the queue
        _size--;
        cvPush.notify_one();
        return 0;
    }

    // Method to return True/False based on queue is empty or not
    bool empty()
    {
        std::lock_guard<std::mutex> gLock(_mtx);
        return _queue.empty();
    }

    // Return the current size of the queue
    int size()
    {
        std::lock_guard<std::mutex> gLock(_mtx);
        return _size;
    }

  private:
    // FIFO type vector to store the the msgs
    std::queue<T> _queue;

    // Mutex to avoid data race
    std::mutex _mtx;

    // Conditional Varible
    std::condition_variable _cond;
    std::condition_variable cvPush;
    std::condition_variable cvPop;
    // Current size of the queue
    int _size{0};
    int maxSize;
};

#endif
