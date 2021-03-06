/*
* Copyright 2018-2019 Redis Labs Ltd. and Contributors
*
* This file is available under the Apache License, Version 2.0,
* modified with the Commons Clause restriction.
*/

#ifndef QUERY_GRAPH_H_
#define QUERY_GRAPH_H_

#include "node.h"
#include "edge.h"
#include "graph.h"
#include "../rmutil/vector.h"
#include "../resultset/resultset_statistics.h"

#define DEFAULT_GRAPH_CAP 32 /* Number of edges/nodes within the graph. */

typedef struct {
    Node **nodes;
    Edge **edges;
    char **node_aliases;
    char **edge_aliases;
    size_t node_count;
    size_t edge_count;
    size_t node_cap;
    size_t edge_cap;
} QueryGraph;

QueryGraph* QueryGraph_New();
QueryGraph* NewQueryGraph_WithCapacity(size_t node_cap, size_t edge_cap);

/* Given AST's MATCH node constructs a graph
 * representing queried entities and the relationships
 * between them. */
void BuildQueryGraph(RedisModuleCtx *ctx, const Graph *g, const char *graph_name, QueryGraph *query_graph, Vector *entities);

/* Checks if graph contains given node
 * Returns 1 if so, 0 otherwise */ 
int QueryGraph_ContainsNode(const QueryGraph *graph, const Node *node);

int QueryGraph_ContainsEdge(const QueryGraph *graph, const Edge *edge);

/* Retrieves node from graph */
Node* QueryGraph_GetNodeById(const QueryGraph *g, long int id);

/* Retrieves node from graph */
Edge* QueryGraph_GetEdgeById(const QueryGraph *g, long int id);

/* Search the graph for a node with given alias */
Node* QueryGraph_GetNodeByAlias(const QueryGraph *g, const char *alias);

/* Search the graph for an edge with given alias */
Edge* QueryGraph_GetEdgeByAlias(const QueryGraph *g, const char *alias);

/* Search for either node/edge with given alias. */
GraphEntity* QueryGraph_GetEntityByAlias(const QueryGraph *g, const char *alias);

/* Adds a new node to the graph */
void QueryGraph_AddNode(QueryGraph* g, Node *n, char *alias);

/* Adds a new edge to the graph */
void QueryGraph_ConnectNodes(QueryGraph *g, Node *src, Node *dest, Edge *e, char *edge_alias);

/* Finds a node with given input degree */
Vector* QueryGraph_GetNDegreeNodes(const QueryGraph *g, int degree);

/* Looks up node's alias within the graph */
char* QueryGraph_GetNodeAlias(const QueryGraph *g, const Node *n);

/* Looks up edge's alias within the graph */
char* QueryGraph_GetEdgeAlias(const QueryGraph *g, const Edge *e);

GraphEntity** QueryGraph_GetEntityRef(const QueryGraph *g, const char *alias);
Node** QueryGraph_GetNodeRef(const QueryGraph *g, const Node *n);
Edge** QueryGraph_GetEdgeRef(const QueryGraph *g, const Edge *e);

/* Saves every entity within the query graph into the actual graph.
 * return statistics regarding the number of entities create and properties set. */
ResultSetStatistics CommitGraph(RedisModuleCtx *ctx, const QueryGraph *qg, Graph *g, const char *graph_name);

/* Frees entire graph */
void QueryGraph_Free(QueryGraph* g);

#endif