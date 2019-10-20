/* This work is licensed under a Creative Commons CCZero 1.0 Universal License.
 * See http://creativecommons.org/publicdomain/zero/1.0/ for more information. 
 *
 *    Copyright 2014-2018 (c) Fraunhofer IOSB (Author: Julius Pfrommer)
 *    Copyright 2017 (c) Julian Grothoff
 *    Copyright 2017 (c) Stefan Profanter, fortiss GmbH
 */

#include <open62541/plugin/nodestore_default.h>
#include "ziptree.h"

#if UA_MULTITHREADING >= 100
#define BEGIN_CRITSECT(ctx) UA_LOCK(ctx->lock)
#define END_CRITSECT(ctx) UA_UNLOCK(ctx->lock)
#else
#define BEGIN_CRITSECT(ctx) do {} while(0)
#define END_CRITSECT(ctx) do {} while(0)
#endif

/* container_of */
#define container_of(ptr, type, member) \
    (type *)((uintptr_t)ptr - offsetof(type,member))

struct NodeEntry;
typedef struct NodeEntry NodeEntry;

struct NodeEntry {
    ZIP_ENTRY(NodeEntry) zipfields;
    UA_UInt32 nodeIdHash;
    UA_UInt16 refCount; /* How many consumers have a reference to the node? */
    UA_Boolean deleted; /* Node was marked as deleted and can be deleted when refCount == 0 */
    NodeEntry *orig;    /* If a copy is made to replace a node, track that we
                         * replace only the node from which the copy was made.
                         * Important for concurrent operations. */
    UA_NodeId nodeId; /* This is actually a UA_Node that also starts with a NodeId */
};

/* Absolute ordering for NodeIds */
static enum ZIP_CMP
cmpNodeId(const void *a, const void *b) {
    const NodeEntry *aa = (const NodeEntry*)a;
    const NodeEntry *bb = (const NodeEntry*)b;

    /* Compare hash */
    if(aa->nodeIdHash < bb->nodeIdHash)
        return ZIP_CMP_LESS;
    if(aa->nodeIdHash > bb->nodeIdHash)
        return ZIP_CMP_MORE;

    /* Compore nodes in detail */
    return (enum ZIP_CMP)UA_NodeId_order(&aa->nodeId, &bb->nodeId);
}

ZIP_HEAD(NodeTree, NodeEntry);
typedef struct NodeTree NodeTree;

typedef struct {
    NodeTree root;
#if UA_MULTITHREADING >= 100
    UA_LOCK_TYPE(lock) /* Protect access */
#endif
} ZipContext;

ZIP_PROTTYPE(NodeTree, NodeEntry, NodeEntry)
ZIP_IMPL(NodeTree, NodeEntry, zipfields, NodeEntry, zipfields, cmpNodeId)

static NodeEntry *
newEntry(UA_NodeClass nodeClass) {
    size_t size = sizeof(NodeEntry) - sizeof(UA_NodeId);
    switch(nodeClass) {
    case UA_NODECLASS_OBJECT:
        size += sizeof(UA_ObjectNode);
        break;
    case UA_NODECLASS_VARIABLE:
        size += sizeof(UA_VariableNode);
        break;
    case UA_NODECLASS_METHOD:
        size += sizeof(UA_MethodNode);
        break;
    case UA_NODECLASS_OBJECTTYPE:
        size += sizeof(UA_ObjectTypeNode);
        break;
    case UA_NODECLASS_VARIABLETYPE:
        size += sizeof(UA_VariableTypeNode);
        break;
    case UA_NODECLASS_REFERENCETYPE:
        size += sizeof(UA_ReferenceTypeNode);
        break;
    case UA_NODECLASS_DATATYPE:
        size += sizeof(UA_DataTypeNode);
        break;
    case UA_NODECLASS_VIEW:
        size += sizeof(UA_ViewNode);
        break;
    default:
        return NULL;
    }
    NodeEntry *entry = (NodeEntry*)UA_calloc(1, size);
    if(!entry)
        return NULL;
    UA_Node *node = (UA_Node*)&entry->nodeId;
    node->nodeClass = nodeClass;
    return entry;
}

static void
deleteEntry(NodeEntry *entry) {
    UA_Node_clear((UA_Node*)&entry->nodeId);
    UA_free(entry);
}

static void
cleanupEntry(NodeEntry *entry) {
    if(entry->deleted && entry->refCount == 0)
        deleteEntry(entry);
}

/***********************/
/* Interface functions */
/***********************/

/* Not yet inserted into the ZipContext */
static UA_Node *
zipNsNewNode(void *nsCtx, UA_NodeClass nodeClass) {
    NodeEntry *entry = newEntry(nodeClass);
    if(!entry)
        return NULL;
    return (UA_Node*)&entry->nodeId;
}

/* Not yet inserted into the ZipContext */
static void
zipNsDeleteNode(void *nsCtx, UA_Node *node) {
    deleteEntry(container_of(node, NodeEntry, nodeId));
}

static const UA_Node *
zipNsGetNode(void *nsCtx, const UA_NodeId *nodeId) {
    ZipContext *ns = (ZipContext*)nsCtx;
    BEGIN_CRITSECT(ns);
    NodeEntry dummy;
    dummy.nodeIdHash = UA_NodeId_hash(nodeId);
    dummy.nodeId = *nodeId;
    NodeEntry *entry = ZIP_FIND(NodeTree, &ns->root, &dummy);
    if(!entry) {
        END_CRITSECT(ns);
        return NULL;
    }
    ++entry->refCount;
    END_CRITSECT(ns);
    return (const UA_Node*)&entry->nodeId;
}

static void
zipNsReleaseNode(void *nsCtx, const UA_Node *node) {
    if(!node)
        return;
#if UA_MULTITHREADING >= 100
        ZipContext *ns = (ZipContext*)nsCtx;
#endif
    BEGIN_CRITSECT(ns);
    NodeEntry *entry = container_of(node, NodeEntry, nodeId);
    UA_assert(entry->refCount > 0);
    --entry->refCount;
    cleanupEntry(entry);
    END_CRITSECT(ns);
}

static UA_StatusCode
zipNsGetNodeCopy(void *nsCtx, const UA_NodeId *nodeId,
                         UA_Node **outNode) {
    /* Find the node */
    const UA_Node *node = zipNsGetNode(nsCtx, nodeId);
    if(!node)
        return UA_STATUSCODE_BADNODEIDUNKNOWN;

    /* Create the new entry */
    NodeEntry *ne = newEntry(node->nodeClass);
    if(!ne) {
        zipNsReleaseNode(nsCtx, node);
        return UA_STATUSCODE_BADOUTOFMEMORY;
    }

    /* Copy the node content */
    UA_Node *nnode = (UA_Node*)&ne->nodeId;
    UA_StatusCode retval = UA_Node_copy(node, nnode);
    zipNsReleaseNode(nsCtx, node);
    if(retval != UA_STATUSCODE_GOOD) {
        deleteEntry(ne);
        return retval;
    }

    ne->orig = container_of(node, NodeEntry, nodeId);
    *outNode = nnode;
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode
zipNsInsertNode(void *nsCtx, UA_Node *node, UA_NodeId *addedNodeId) {
    NodeEntry *entry = container_of(node, NodeEntry, nodeId);
    ZipContext *ns = (ZipContext*)nsCtx;
    BEGIN_CRITSECT(ns);

    /* Ensure that the NodeId is unique */
    NodeEntry dummy;
    dummy.nodeId = node->nodeId;
    if(node->nodeId.identifierType == UA_NODEIDTYPE_NUMERIC &&
       node->nodeId.identifier.numeric == 0) {
        do { /* Create a random nodeid until we find an unoccupied id */
            node->nodeId.identifier.numeric = UA_UInt32_random();
            dummy.nodeId.identifier.numeric = node->nodeId.identifier.numeric;
            dummy.nodeIdHash = UA_NodeId_hash(&node->nodeId);
        } while(ZIP_FIND(NodeTree, &ns->root, &dummy));
    } else {
        dummy.nodeIdHash = UA_NodeId_hash(&node->nodeId);
        if(ZIP_FIND(NodeTree, &ns->root, &dummy)) { /* The nodeid exists */
            deleteEntry(entry);
            END_CRITSECT(ns);
            return UA_STATUSCODE_BADNODEIDEXISTS;
        }
    }

    /* Copy the NodeId */
    if(addedNodeId) {
        UA_StatusCode retval = UA_NodeId_copy(&node->nodeId, addedNodeId);
        if(retval != UA_STATUSCODE_GOOD) {
            deleteEntry(entry);
            END_CRITSECT(ns);
            return retval;
        }
    }

    /* Insert the node */
    entry->nodeIdHash = dummy.nodeIdHash;
    ZIP_INSERT(NodeTree, &ns->root, entry, ZIP_FFS32(UA_UInt32_random()));
    END_CRITSECT(ns);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode
zipNsReplaceNode(void *nsCtx, UA_Node *node) {
    /* Find the node */
    const UA_Node *oldNode = zipNsGetNode(nsCtx, &node->nodeId);
    if(!oldNode) {
        deleteEntry(container_of(node, NodeEntry, nodeId));
        return UA_STATUSCODE_BADNODEIDUNKNOWN;
    }

    /* Test if the copy is current */
    NodeEntry *entry = container_of(node, NodeEntry, nodeId);
    NodeEntry *oldEntry = container_of(oldNode, NodeEntry, nodeId);
    if(oldEntry != entry->orig) {
        /* The node was already updated since the copy was made */
        deleteEntry(entry);
        zipNsReleaseNode(nsCtx, oldNode);
        return UA_STATUSCODE_BADINTERNALERROR;
    }

    /* Replace */
    ZipContext *ns = (ZipContext*)nsCtx;
    BEGIN_CRITSECT(ns);
    ZIP_REMOVE(NodeTree, &ns->root, oldEntry);
    entry->nodeIdHash = oldEntry->nodeIdHash;
    ZIP_INSERT(NodeTree, &ns->root, entry, ZIP_RANK(entry, zipfields));
    oldEntry->deleted = true;
    END_CRITSECT(ns);

    zipNsReleaseNode(nsCtx, oldNode);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode
zipNsRemoveNode(void *nsCtx, const UA_NodeId *nodeId) {
    ZipContext *ns = (ZipContext*)nsCtx;
    BEGIN_CRITSECT(ns);
    NodeEntry dummy;
    dummy.nodeIdHash = UA_NodeId_hash(nodeId);
    dummy.nodeId = *nodeId;
    NodeEntry *entry = ZIP_FIND(NodeTree, &ns->root, &dummy);
    if(!entry) {
        END_CRITSECT(ns);
        return UA_STATUSCODE_BADNODEIDUNKNOWN;
    }
    ZIP_REMOVE(NodeTree, &ns->root, entry);
    entry->deleted = true;
    cleanupEntry(entry);
    END_CRITSECT(ns);
    return UA_STATUSCODE_GOOD;
}

struct VisitorData {
    UA_NodestoreVisitor visitor;
    void *visitorContext;
};

static void
nodeVisitor(NodeEntry *entry, void *data) {
    struct VisitorData *d = (struct VisitorData*)data;
    d->visitor(d->visitorContext, (UA_Node*)&entry->nodeId);
}

static void
zipNsIterate(void *nsCtx, UA_NodestoreVisitor visitor,
             void *visitorCtx) {
    struct VisitorData d;
    d.visitor = visitor;
    d.visitorContext = visitorCtx;
    ZipContext *ns = (ZipContext*)nsCtx;
    BEGIN_CRITSECT(ns);
    ZIP_ITER(NodeTree, &ns->root, nodeVisitor, &d);
    END_CRITSECT(ns);
}

static void
deleteNodeVisitor(NodeEntry *entry, void *data) {
    deleteEntry(entry);
}

/***********************/
/* Nodestore Lifecycle */
/***********************/

static void
zipNsClear(void *nsCtx) {
    if (!nsCtx)
        return;
    ZipContext *ns = (ZipContext*)nsCtx;
#if UA_MULTITHREADING >= 100
    UA_LOCK_DESTROY(ns->lock);
#endif
    ZIP_ITER(NodeTree, &ns->root, deleteNodeVisitor, NULL);
    UA_free(ns);
}

UA_StatusCode
UA_Nodestore_ZipTree(UA_Nodestore *ns) {
    /* Allocate and initialize the context */
    ZipContext *ctx = (ZipContext*)UA_malloc(sizeof(ZipContext));
    if(!ctx)
        return UA_STATUSCODE_BADOUTOFMEMORY;
#if UA_MULTITHREADING >= 100
    UA_LOCK_INIT(ctx->lock)
#endif

    ZIP_INIT(&ctx->root);

    /* Populate the nodestore */
    ns->context = (void*)ctx;
    ns->clear = zipNsClear;
    ns->newNode = zipNsNewNode;
    ns->deleteNode = zipNsDeleteNode;
    ns->getNode = zipNsGetNode;
    ns->releaseNode = zipNsReleaseNode;
    ns->getNodeCopy = zipNsGetNodeCopy;
    ns->insertNode = zipNsInsertNode;
    ns->replaceNode = zipNsReplaceNode;
    ns->removeNode = zipNsRemoveNode;
    ns->iterate = zipNsIterate;
    
    return UA_STATUSCODE_GOOD;
}