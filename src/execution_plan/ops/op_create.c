/*
* Copyright 2018-2019 Redis Labs Ltd. and Contributors
*
* This file is available under the Apache License, Version 2.0,
* modified with the Commons Clause restriction.
*/

#include "../../stores/store.h"
#include "op_create.h"
#include <assert.h>

void _SetModifiedEntities(OpCreate *op) {
    /* Determin which entities are modified by create op.
     * Search uninitialized nodes. */
    QueryGraph* graph = op->qg;
    size_t create_entity_count = Vector_Size(op->ast->createNode->graphEntities);
    op->nodes_to_create = malloc(sizeof(NodeCreateCtx) * create_entity_count);
    op->edges_to_create = malloc(sizeof(EdgeCreateCtx) * create_entity_count);

    int node_idx = 0;
    int edge_idx = 0;

    /* For every entity within the CREATE clause see if it's also mentioned 
     * within the MATCH clause. */
    for(int i = 0; i < create_entity_count; i++) {
        AST_GraphEntity *create_ge;
        Vector_Get(op->ast->createNode->graphEntities, i, &create_ge);
        
        /* See if current entity is in MATCH clause. */
        AST_GraphEntity *match_ge = MatchClause_GetEntity(op->ast->matchNode, create_ge->alias);
        if(!match_ge) {
            /* Entity is not in MATCH clause. */
            AST_GraphEntity *ge = create_ge;
            if(ge->t == N_ENTITY) {
                // Node.
                Node *n = QueryGraph_GetNodeByAlias(op->qg, ge->alias);
                if(n->id == 0) continue;   // Node already marked for creation.
                Node **ppn = QueryGraph_GetNodeRef(op->qg, n);
                op->nodes_to_create[node_idx].original_node = n;
                op->nodes_to_create[node_idx].original_node_ref = ppn;
                n->id = 0;  /* Mark node. */
                node_idx++;
            } else {
                // Edge.
                Edge *e = QueryGraph_GetEdgeByAlias(op->qg, ge->alias);
                if(e->id == 0) continue;   // Edge already marked for creation.
                op->edges_to_create[edge_idx].original_edge = e;
                op->edges_to_create[edge_idx].original_edge_ref = QueryGraph_GetEdgeRef(op->qg, e);
                assert(QueryGraph_ContainsNode(graph, e->src));
                assert(QueryGraph_ContainsNode(graph, e->dest));
                op->edges_to_create[edge_idx].src_node_alias = QueryGraph_GetNodeAlias(graph, e->src);
                op->edges_to_create[edge_idx].dest_node_alias = QueryGraph_GetNodeAlias(graph, e->dest);
                e->id = 0;  /* Mark edge. */
                edge_idx++;
            }
        }
    }
    
    /* Restore IDs. */
    for(int i = 0; i < node_idx; i++) {
        op->nodes_to_create[i].original_node->id = INVALID_ENTITY_ID;
    }
    for(int i = 0; i < edge_idx; i++) {
        op->edges_to_create[i].original_edge->id = INVALID_ENTITY_ID;
    }

    op->node_count = node_idx;
    op->edge_count = edge_idx;
    /* Create must modify atleast one entity. */
    assert((op->node_count + op->edge_count) > 0);
}

OpBase* NewCreateOp(RedisModuleCtx *ctx, AST_Query *ast, Graph *g, QueryGraph *qg, const char *graph_name, int request_refresh, ResultSet *result_set) {
    OpCreate *op_create = calloc(1, sizeof(OpCreate));

    op_create->ctx = ctx;
    op_create->ast = ast;
    op_create->g = g;
    op_create->qg = qg;
    op_create->graph_name = graph_name;
    
    op_create->nodes_to_create = NULL;
    op_create->node_count = 0;
    op_create->edges_to_create = NULL;
    op_create->edge_count = 0;
    op_create->created_nodes = NewVector(Node*, 0);
    op_create->created_edges = NewVector(Edge*, 0);
    op_create->result_set = result_set;    

    _SetModifiedEntities(op_create);

    // Set our Op operations
    OpBase_Init(&op_create->op);
    op_create->op.name = "Create";
    op_create->op.type = OPType_CREATE;
    op_create->op.consume = OpCreateConsume;
    op_create->op.reset = OpCreateReset;
    op_create->op.free = OpCreateFree;

    return (OpBase*)op_create;
}

void _CreateNodes(OpCreate *op) {
    for(int i = 0; i < op->node_count; i++) {
        /* Get specified node to create. */
        Node *n = op->nodes_to_create[i].original_node;

        /* Create a new node. */
        long id = Graph_NodeCount(op->g) + Vector_Size(op->created_nodes);
        Node *node = Node_New(id, n->label);

        /* Add properties.*/
        if(n->prop_count > 0) {
            char *keys[n->prop_count];
            SIValue vals[n->prop_count];
            for(int prop_idx = 0; prop_idx < n->prop_count; prop_idx++) {
                EntityProperty prop = n->properties[prop_idx];
                keys[prop_idx] = prop.name;
                vals[prop_idx] = prop.value;
            }
            Node_Add_Properties(node, n->prop_count, keys, vals);
        }

        /* Save node for later insertion. */
        Vector_Push(op->created_nodes, node);

        /* Update query graph with new node. */
        *(op->nodes_to_create[i].original_node_ref) = node;
    }
}

void _CreateEdges(OpCreate *op) {
    for(int i = 0; i < op->edge_count; i++) {
        /* Get specified edge to create. */
        Edge *e = op->edges_to_create[i].original_edge;

        /* Retrieve source and dest nodes. */
        Node *src_node = QueryGraph_GetNodeByAlias(op->qg, op->edges_to_create[i].src_node_alias);
        Node *dest_node = QueryGraph_GetNodeByAlias(op->qg, op->edges_to_create[i].dest_node_alias);

        /* Create the actual edge. */
        Edge *edge = Edge_New(0, src_node, dest_node, e->relationship);

        /* Add properties.*/
        char *keys[e->prop_count];
        SIValue vals[e->prop_count];
        for(int prop_idx = 0; prop_idx < e->prop_count; prop_idx++) {
            EntityProperty prop = e->properties[prop_idx];
            keys[prop_idx] = prop.name;
            vals[prop_idx] = prop.value;
        }
        Edge_Add_Properties(edge, e->prop_count, keys, vals);

        // Node_ConnectNode(src_node, dest_node, edge);
        
        /* Save edge for later insertion. */
        Vector_Push(op->created_edges, edge);

        /* Update query graph with new node. */
        *(op->edges_to_create[i].original_edge_ref) = edge;
    }
}

/* Commit insertions. */
void _CommitNewEntities(OpCreate *op) {
    RedisModuleCtx *ctx = op->ctx;
    size_t node_count = Vector_Size(op->created_nodes);
    size_t edge_count = Vector_Size(op->created_edges);

    if(node_count > 0) {
        int labels[node_count];
        LabelStore *allStore = LabelStore_Get(ctx, STORE_NODE, op->graph_name, NULL);

        for(int i = 0; i < node_count; i++) {
            Node *n;
            Vector_Get(op->created_nodes, i, &n);
            LabelStore *store = NULL;
            const char *label = n->label;
            if(label == NULL) {
               labels[i] = GRAPH_NO_LABEL; 
            } else {
                store = LabelStore_Get(ctx, STORE_NODE, op->graph_name, label);                
                if(store == NULL) {
                    int label_id = Graph_AddLabelMatrix(op->g);
                    store = LabelStore_New(ctx, STORE_NODE, op->graph_name, label, label_id);
                    op->result_set->stats.labels_added++;
                }
                labels[i] = store->id;
            }

            if(n->prop_count > 0) {
                char *properties[n->prop_count];
                for(int j = 0; j < n->prop_count; j++) properties[j] = n->properties[j].name;
                if(label) LabelStore_UpdateSchema(store, n->prop_count, properties);
                LabelStore_UpdateSchema(allStore, n->prop_count, properties);
            }
        }

        NodeIterator *it;
        size_t graph_node_count = Graph_NodeCount(op->g);
        Graph_CreateNodes(op->g, node_count, labels, &it);

        for(int i = 0; i < node_count; i++) {
            Node *new_node = NodeIterator_Next(it);
            Node *temp_node;
            Vector_Get(op->created_nodes, i, &temp_node);

            new_node->properties = temp_node->properties;
            new_node->prop_count = temp_node->prop_count;
            new_node->id = graph_node_count + i;
            temp_node->id = new_node->id;   /* Formed edges refer to temp_node. */
            temp_node->properties = NULL;   /* Do not free temp_node's property set. */
            op->result_set->stats.properties_set += new_node->prop_count;
        }

        op->result_set->stats.nodes_created = node_count;
    }

    if(edge_count > 0) {
        GrB_Index connections[edge_count * 3];

        for(int i = 0; i < edge_count; i++) {
            Edge *e;
            int con_idx = i*3;
            Vector_Pop(op->created_edges, &e);

            connections[con_idx] = e->src->id;
            connections[con_idx + 1] = e->dest->id;
            
            if(!e->relationship) {
                connections[con_idx + 2] = GRAPH_NO_RELATION;
            } else {
                LabelStore *s = LabelStore_Get(ctx, STORE_EDGE, op->graph_name, e->relationship);
                if(s != NULL) connections[con_idx + 2] = s->id;
                else {
                    int relation_id = Graph_AddRelationMatrix(op->g);
                    LabelStore_New(op->ctx, STORE_EDGE, op->graph_name, e->relationship, relation_id);
                    connections[con_idx + 2] = relation_id;
                }
            }
            Edge_Free(e);
        }

        Graph_ConnectNodes(op->g, edge_count*3, connections);
        op->result_set->stats.relationships_created = edge_count;
    }

    for(int i = 0; i < node_count; i++) {
        Node *temp_node;
        Vector_Get(op->created_nodes, i, &temp_node);
        Node_Free(temp_node);
    }
}

OpResult OpCreateConsume(OpBase *opBase, QueryGraph* graph) {
    OpResult res = OP_OK;
    OpCreate *op = (OpCreate*)opBase;

    if(op->op.childCount) {
        OpBase *child = op->op.children[0];
        res = child->consume(child, graph);
    }

    if(res == OP_OK) {
        /* Create entities. */
        _CreateNodes(op);
        _CreateEdges(op);
    }

    if(op->op.childCount == 0) return OP_DEPLETED;
    else return res;
}

OpResult OpCreateReset(OpBase *ctx) {
    OpCreate *op = (OpCreate*)ctx;

    for(int i = 0; i < op->node_count; i++) {
        *(op->nodes_to_create[i].original_node_ref) = op->nodes_to_create[i].original_node;
    }

    for(int i = 0; i < op->edge_count; i++) {
        *(op->edges_to_create[i].original_edge_ref) = op->edges_to_create[i].original_edge;
    }

    return OP_OK;
}

void OpCreateFree(OpBase *ctx) {
    OpCreate *op = (OpCreate*)ctx;
    _CommitNewEntities(op);

    if(op->nodes_to_create != NULL) {
        free(op->nodes_to_create);
    }

    if(op->edges_to_create != NULL) {
        free(op->edges_to_create);
    }
}
