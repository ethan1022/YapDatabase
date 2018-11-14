/**
 * Copyright Deusty LLC.
**/

#import <Foundation/Foundation.h>

#import "YapDatabaseCloudCorePipelineDelegate.h"
#import "YapDatabaseCloudCoreGraph.h"
#import "YapDatabaseCloudCoreOperation.h"
#import "YapDatabaseCloudCore.h"

typedef NS_ENUM(NSInteger, YDBCloudCorePipelineAlgorithm) {
	
	/**
	 * This is the default algorithm if you don't explicitly pick one.
	 * It is HIGHLY recommended you start with this algorithm, until you become more advanced.
	 *
	 * The "Commit Graph" algorithm works as follows:
	 *
	 * - all operations added within a specific commit are added to their own "graph"
	 * - the pipeline will execute each graph 1-at-a-time
	 * - this ensures that graphs are completed in commit order
	 *
	 * That is, if a pipeline contains 2 graphs:
	 * - graph "A" - representing operations from commit #32
	 * - graph "B" - represending operations from commit #33
	 *
	 * Then the pipeline will ensure that ALL operations from graphA are either completed or skipped
	 * before ANY operations from graphB start.
	 *
	 * This is the safest option because it means:
	 * - you only have to think about operation dependencies within the context of a single commit
	 * - the pipeline ensures the cloud moves from commit to commit (just as occurred locally)
	**/
	YDBCloudCorePipelineAlgorithm_CommitGraph = 0,
	
	/**
	 * This is and ADVANCED algorithm that is only recommended after your cloud solution has matured.
	 *
	 * The "Flat Graph" algorithm works as follows:
	 *
	 * - all operations added within a specific commit are added to their own "graph"
	 * - HOWEVER, the pipeline is free to start operations from ANY graph
	 * - and it will do so, while respecting dependencies, priorities & maxConcurrentOperationCount
	 *
	 * In particular, what this means for you is:
	 *
	 * - you MUST create a FORMAL DEPENDENCY GRAPH (think: state diagram for dependencies)
	 *
	 * That is:
	 * - given any possible operation (opA) in commitA
	 * - and given any possible operation (opB) in commitB
	 * - your formal dependency graph must determine if opB should depend on opA
	 *
	 * The recommended way of implementing your formal dependency graph is by
	 * subclassing YapDatabaseCloudCoreTransaction & overriding the method
	 * `processOperations:inPipeline:withGraphIdx:`.
	**/
	YDBCloudCorePipelineAlgorithm_FlatGraph = 1
};

typedef NS_ENUM(NSInteger, YDBCloudCoreOperationStatus) {
	
	/**
	 * Pending means that the operation is queued in the pipeline,
	 * and may be released to the delegate when ready.
	 * 
	 * If an operation fails, the PipelineDelegate may re-queue the operation by marking its status as pending.
	 * This gives control over the operation back to the pipeline,
	 * and it will dispatch it to the PipelineDelegate again when ready.
	**/
	YDBCloudOperationStatus_Pending = 0,
	
	/**
	 * The operation has been started.
	 * I.e. has been handed to the PipelineDelegate via 'startOperation::'.
	**/
	YDBCloudOperationStatus_Active,
	
	/**
	 * Until an operation is marked as either completed or skipped,
	 * the pipeline will act as if the operation is still in progress.
	 * 
	 * In order to mark an operation as completed or skipped, the following must be used:
	 * - [YapDatabaseCloudCoreTransaction completeOperation:]
	 * - [YapDatabaseCloudCoreTransaction skipOperation:]
	 * 
	 * These methods allow the system to delete the operation from the internal sqlite table.
	**/
	YDBCloudOperationStatus_Completed,
	YDBCloudOperationStatus_Skipped,
};

/**
 * This notification is posted whenever the operations in the pipeline's queue have changed.
 * That is, one of the following have occurred:
 * - One or more operations were removed from the queue (completed or skipped)
 * - One or more operations were added to the queue (added or inserted)
 * - One or more operations were modified
 * 
 * This notification is posted to the main thread.
**/
extern NSString *const YDBCloudCorePipelineQueueChangedNotification;

/**
 * This notification is posted whenever the suspendCount changes.
 * This notification is posted to the main thread.
**/
extern NSString *const YDBCloudCorePipelineSuspendCountChangedNotification;

/**
 * This notification is posted whenever the isActive status changes.
 * This notification is posted to the main thread.
**/
extern NSString *const YDBCloudCorePipelineActiveStatusChangedNotification;

/**
 * A "pipeline" represents a queue of operations for syncing with a cloud server.
 * It operates by managing a series of "graphs".
 * 
 * Generally speaking, a graph is all the cloud operations that were generated in a single commit (for a
 * specific pipeline). Within the graph are the various operations with their different dependencies & priorities.
 * The operations within a graph will be executed in accordance with the set dependencies & priorities.
 * 
 * The pipeline manages executing the operations within each graph.
**/
@interface YapDatabaseCloudCorePipeline : NSObject

/**
 * Initializes a pipeline instance with the given name and delegate.
 * After creating a pipeline instance, you need to register it via [YapDatabaseCloudCore registerPipeline:].
**/
- (instancetype)initWithName:(NSString *)name
                    delegate:(id <YapDatabaseCloudCorePipelineDelegate>)delegate;

/**
 * Initializes a pipeline instance with the given name and delegate.
 * Additionally, you may choose to use an advanced algorithm such as FlatGraph.
 *
 * After creating a pipeline instance, you need to register it via [YapDatabaseCloudCore registerPipeline:].
**/
- (instancetype)initWithName:(NSString *)name
                   algorithm:(YDBCloudCorePipelineAlgorithm)algorithm
                    delegate:(id <YapDatabaseCloudCorePipelineDelegate>)delegate;

@property (nonatomic, copy, readonly) NSString *name;
@property (nonatomic, assign, readonly) YDBCloudCorePipelineAlgorithm algorithm;
@property (nonatomic, weak, readonly) id <YapDatabaseCloudCorePipelineDelegate> delegate;

@property (atomic, weak, readonly) YapDatabaseCloudCore *owner;

#pragma mark Configuration

/**
 * If you decide to rename a pipeline, you should be sure to set the previousNames property.
 * This is to ensure that operations (from previous app launches) that were tagged with the previous pipeline name
 * can be properly migrated to the new pipeline name.
 * 
 * This property must be set before the pipeline is registered.
**/
@property (nonatomic, copy, readwrite) NSSet *previousNames;

/**
 * This value is the maximum number of operations that will be assigned to the delegate at any one time.
 * 
 * The pipeline keeps track of operations that have been assigned to the delegate (via startOperation:forPipeline:),
 * and will delay assigning any more operations once the maxConcurrentOperationCount has been reached.
 * Once an operation is completed (or skipped), the pipeline will automatically resume.
 * 
 * Of course, the delegate is welcome to perform its own concurrency restriction.
 * For example, via NSURLSessionConfiguration.HTTPMaximumConnectionsPerHost.
 * In which case it may simply set this to a high enough value that it won't interfere with its own implementation.
 * 
 * This value may be changed at anytime.
 *
 * The default value is 8.
**/
@property (atomic, assign, readwrite) NSUInteger maxConcurrentOperationCount;

#pragma mark Operation Searching

/**
 * Searches for an operation with the given UUID.
 *
 * @return The corresponding operation, if found. Otherwise nil.
**/
- (YapDatabaseCloudCoreOperation *)operationWithUUID:(NSUUID *)uuid;

/**
 * Returns a list of operations in state 'YDBCloudOperationStatus_Active'.
**/
- (NSArray<YapDatabaseCloudCoreOperation *> *)activeOperations;

/**
 * Enumerates the queued operations.
 *
 * This is useful for finding operation.
 * For example, you might use this to search for an upload operation with a certain cloudPath.
**/
- (void)enumerateOperationsUsingBlock:(void (^)(YapDatabaseCloudCoreOperation *operation,
                                                NSUInteger graphIdx, BOOL *stop))enumBlock;

/**
 * Returns the number of graphs queued in the pipeline.
 * Each graph represents the operations from a particular commit.
**/
- (NSUInteger)graphCount;

/**
 * Each graph corresponds to a particular commit.
 * And YapDatabase keeps track of commit numbers via its `snapshot` property.
 *
 * The `snapshot` can be understood as a commit number that gets incremented during
 * every read-write transaction (for which actual changes were made to the database).
 * Note also that the `snapshot` is stored in the database.
 * So it's a persistent number that continually increments across app launches.
 * (i.e. does NOT reset to zero on app re-launch, but rather continues incrementing where it left off)
 *
 * The snapshot number is known for each graph.
 * However, most API's deal instead with "graph indexes" (graphIdx).
 * This is because the graph index is generally seen as more useful and intuitive.
 *
 * For example, imagine the following scenario:
 * - commit 44 creates 2 operations
 * - commit 45 & 46 create 0 operations
 * - commit 47 creates 2 operations
 *
 * Intuitively, we see that we have 2 graphs, each with 2 operations.
 * And we think of the graphs as being in an array.
 * If we enumerate the operations, it becomes more intuitive to be given graph indexes.
 * So index 0 is the first graph in the array, index 1 comes next, etc...
 * Further, in terms of the graphs, the corresponding snapshot numbers are non-sequential.
 * The first graph is 44... and then jumps to 47 !
 * 
 * Hence, most API's deal with graphs as arrays, and expose their index within the array.
 * However, sometimes it's useful to get the persistent snapshot number that corresponds to a graph.
**/
- (BOOL)getSnapshot:(uint64_t *)snapshotPtr forGraphIndex:(NSUInteger)graphIdx;
- (BOOL)getGraphIndex:(NSUInteger *)graphIdxPtr forSnapshot:(uint64_t)snapshot;

#pragma mark Operation Status

/**
 * Returns the current status for the given operation.
**/
- (YDBCloudCoreOperationStatus)statusForOperationWithUUID:(NSUUID *)opUUID;

/**
 * Typically you are strongly discouraged from manually starting an operation.
 * You should allow the pipeline to mange the queue, and only start operations when told to.
 *
 * However, there is one particular edge case in which is is unavoidable: background network tasks.
 * If the app is relaunched, and you discover there are network tasks from a previous app session,
 * you'll obviously want to avoid starting the corresponding operation again.
 * In this case, you should use this method to inform the pipeline that the operation is already started.
**/
- (void)setStatusAsActiveForOperationWithUUID:(NSUUID *)opUUID;

/**
 * The PipelineDelegate may invoke this method to reset a failed operation.
 * This gives control over the operation back to the pipeline,
 * and it will dispatch it back to the PipelineDelegate again when ready.
**/
- (void)setStatusAsPendingForOperationWithUUID:(NSUUID *)opUUID;

/**
 * The PipelineDelegate may invoke this method to reset a failed operation,
 * and simultaneously tell the pipeline to delay retrying it again for a period of time.
 *
 * This is typically used when implementing retry logic such as exponential backoff.
 * It works by setting a hold on the operation to [now dateByAddingTimeInterval:delay].
**/
- (void)setStatusAsPendingForOperationWithUUID:(NSUUID *)opUUID
                                    retryDelay:(NSTimeInterval)delay;

#pragma mark Operation Hold

/**
 * Returns the current hold for the operation, or nil if there is no hold.
**/
- (NSDate *)holdDateForOperationWithUUID:(NSUUID *)opUUID;

/**
 * And operation can be put on "hold" until a specified date.
 * This is typically used in conjunction with retry logic such as exponential backoff.
 * 
 * The operation won't be delegated again until the given date.
 * You can pass a nil date to remove a hold on an operation.
 * 
 * @see setStatusAsPendingForOperation:withRetryDelay:
**/
- (void)setHoldDate:(NSDate *)date forOperationWithUUID:(NSUUID *)opUUID;


#pragma mark Suspend & Resume

/**
 * Returns YES if the upload operation queue is suspended.
 *
 * @see suspend
 * @see resume
**/
@property (atomic, readonly) BOOL isSuspended;

/**
 * Returns the current suspendCount.
 * If the suspendCount is zero, that means isSuspended == NO;
 * if the suspendCount is non-zero, that means isSuspended == YES;
 *
 * @see suspend
 * @see resume
**/
@property (atomic, readonly) NSUInteger suspendCount;

/**
 * Increments the suspendCount.
 * All calls to 'suspend' need to be matched with an equal number of calls to 'resume'.
 * 
 * @return
 *   The new suspend count.
 *   This will be 1 if the pipeline was previously active, and is now suspended due to this call.
 *   Otherwise it will be greater than one, meaning it was previously suspended,
 *   and you just incremented the suspend count.
 * 
 * @see resume
 * @see suspendCount
**/
- (NSUInteger)suspend;

/**
 * This method operates the same as invoking the suspend method the given number of times.
 * That is, it increments the suspend count by the given number.
 *
 * If you invoke this method with a zero parameter,
 * it will simply return the current suspend count, without modifying it.
 *
 * @see suspend
 * @see suspendCount
**/
- (NSUInteger)suspendWithCount:(NSUInteger)suspendCountIncrement;

/**
 * Decrements the suspendCount.
 * All calls to 'suspend' need to be matched with an equal number of calls to 'resume'.
 *
 * @return
 *   The current suspend count.
 *   This will be 0 if the extension was previously suspended, and is now resumed due to this call.
 *   Otherwise it will be greater than one, meaning it's still suspended,
 *   and you just decremented the suspend count.
 *
 * @see suspend
 * @see suspendCount
**/
- (NSUInteger)resume;

#pragma mark Activity

/**
 * A pipeline transitions to the 'active' state when:
 * - There are 1 or more operations in 'YDBCloudOperationStatus_Active' mode.
 *
 * A pipeline transitions to the 'inactive' state when:
 * - There are 0 operations in 'YDBCloudOperationStatus_Active' mode
 * - AND (the pipeline is suspended OR there are no more operations)
 *
 * ^In other words, there may be situations in which there are zero active operations,
 *  due to something like a conflict resolution, however the pipeline is still considered
 *  active because it still has pending operations, and it hasn't been suspended.
**/
@property (atomic, readonly) BOOL isActive;

@end
