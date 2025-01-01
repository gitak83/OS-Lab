#include "types.h"
#include "stat.h"
#include "user.h"
#include "spinlock.h"

// Declare the reentrant lock
struct reetrantlock rlk;

void recursive_lock(int depth) {
    if (depth == 0) {
        return;  // Base case: no more recursion
    }

    // Attempt to acquire the lock
    printf(1, "Depth %d: Attempting to take the lock...\n", depth);
    acquirereentrantlock(&rlk);
    printf(1, "Depth %d: Lock successfully taken!\n", depth);

    // Recursively call the function with reduced depth
    recursive_lock(depth - 1);

    // After the recursive call, release the lock
    printf(1, "Depth %d: Preparing to release the lock...\n", depth);
    releasereentrantlock(&rlk);
    printf(1, "Depth %d: Lock released. Onward to the next step!\n", depth);
}

int main() {
    // Initialize the reentrant lock
    initreentrantlock(&rlk, "Test Lock");

    // Print the start of the test
    printf(1, "Reentrant Lock Test: Let the journey begin\n");

    // Start the recursive lock test with a recursion depth of 3
    recursive_lock(5);

    // Print the completion message
    printf(1, "\nCongratulations! Locking and unlocking done right!\n");

    // Exit the program
    exit();
}
