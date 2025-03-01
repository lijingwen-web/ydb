#include <ydb/library/yql/sql/pg_sql.h>
#include <ydb/library/yql/parser/pg_wrapper/parser.h>
#include <ydb/library/yql/providers/common/provider/yql_provider_names.h>
#include <ydb/library/yql/core/yql_callable_names.h>
#include <ydb/library/yql/parser/pg_catalog/catalog.h>
#include <util/string/builder.h>
#include <util/string/cast.h>
#include <util/generic/scope.h>
#include <util/generic/stack.h>
#include <util/generic/hash_set.h>

#ifdef _WIN32
#define __restrict
#endif

#define TypeName PG_TypeName
#define SortBy PG_SortBy
#undef SIZEOF_SIZE_T
extern "C" {
#include "postgres.h"
#include "nodes/pg_list.h"
#include "nodes/parsenodes.h"
#include "nodes/value.h"
#undef Min
#undef Max
#undef TypeName
#undef SortBy
}

namespace NSQLTranslationPG {

using namespace NYql;

template <typename T>
const T* CastNode(const void* nodeptr, int tag) {
    Y_ENSURE(nodeTag(nodeptr) == tag);
    return static_cast<const T*>(nodeptr);
}

const Node* Expr2Node(const Expr* e) {
    return reinterpret_cast<const Node*>(e);
}

int NodeTag(const Node* node) {
    return nodeTag(node);
}

int NodeTag(const Value& node) {
    return node.type;
}

int IntVal(const Value& node) {
    Y_ENSURE(node.type == T_Integer);
    return intVal(&node);
}

double FloatVal(const Value& node) {
    Y_ENSURE(node.type == T_Float);
    return floatVal(&node);
}

const char* StrFloatVal(const Value& node) {
    Y_ENSURE(node.type == T_Float);
    return strVal(&node);
}

const char* StrVal(const Value& node) {
    Y_ENSURE(node.type == T_String);
    return strVal(&node);
}

int IntVal(const Node* node) {
    Y_ENSURE(node->type == T_Integer);
    return intVal((const Value*)node);
}

double FloatVal(const Node* node) {
    Y_ENSURE(node->type == T_Float);
    return floatVal((const Value*)node);
}

const char* StrFloatVal(const Node* node) {
    Y_ENSURE(node->type == T_Float);
    return strVal((const Value*)node);
}

const char* StrVal(const Node* node) {
    Y_ENSURE(node->type == T_String);
    return strVal((const Value*)node);
}

bool ValueAsString(const Value& val, TString& ret) {
    switch (NodeTag(val)) {
    case T_Integer: {
        ret = ToString(IntVal(val));
        return true;
    }
    case T_Float: {
        ret = StrFloatVal(val);
        return true;
    }
    case T_String: {
        ret = StrVal(val);
        return true;
    }
    case T_Null: {
        ret = "NULL";
        return true;
    }
    default:
        return false;
    }
}

int ListLength(const List* list) {
    return list_length(list);
}

int StrLength(const char* s) {
    return s ? strlen(s) : 0;
}

int StrCompare(const char* s1, const char* s2) {
    return strcmp(s1 ? s1 : "", s2 ? s2 : "");
}

#define CAST_NODE(nodeType, nodeptr) CastNode<nodeType>(nodeptr, T_##nodeType)
#define CAST_NODE_EXT(nodeType, tag, nodeptr) CastNode<nodeType>(nodeptr, tag)
#define LIST_CAST_NTH(nodeType, list, index) CAST_NODE(nodeType, list_nth(list, i))
#define LIST_CAST_EXT_NTH(nodeType, tag, list, index) CAST_NODE_EXT(nodeType, tag, list_nth(list, i))

const Node* ListNodeNth(const List* list, int index) {
    return static_cast<const Node*>(list_nth(list, index));
}

class TConverter : public IPGParseEvents {
public:
    struct TFromDesc {
        TAstNode* Source = nullptr;
        TString Alias;
        TVector<TString> ColNames;
        bool InjectRead = false;
    };

    struct TInsertDesc {
        TAstNode* Sink = nullptr;
        TAstNode* Key = nullptr;
    };

    struct TExprSettings {
        bool AllowColumns = false;
        bool AllowAggregates = false;
        bool AllowOver = false;
        bool AllowReturnSet = false;
        TVector<TAstNode*>* WindowItems = nullptr;
        TString Scope;
    };

    struct TView {
        TString Name;
        TVector<TString> ColNames;
        TAstNode* Source = nullptr;
    };

    using TViews = THashMap<TString, TView>;

    TConverter(TAstParseResult& astParseResult, const NSQLTranslation::TTranslationSettings& settings)
        : AstParseResult(astParseResult)
        , Settings(settings)
        , DqEngineEnabled(Settings.DqDefaultAuto->Allow())
    {
        for (auto& flag : Settings.Flags) {
            if (flag == "DqEngineEnable") {
                DqEngineEnabled = true;
            } else if (flag == "DqEngineForce") {
                DqEngineForce = true;
            }
        }
    }

    void OnResult(const List* raw) {
        AstParseResult.Pool = std::make_unique<TMemoryPool>(4096);
        AstParseResult.Root = ParseResult(raw);
    }

    void OnError(const TIssue& issue) {
        AstParseResult.Issues.AddIssue(issue);
    }

    TAstNode* ParseResult(const List* raw) {
        auto configSource = L(A("DataSource"), QA(TString(NYql::ConfigProviderName)));
        Statements.push_back(L(A("let"), A("world"), L(A(TString(NYql::ConfigureName)), A("world"), configSource,
            QA("OrderedColumns"))));

        DqEnginePgmPos = Statements.size();
        Statements.push_back(configSource);

        for (int i = 0; i < ListLength(raw); ++i) {
           if (!ParseRawStmt(LIST_CAST_NTH(RawStmt, raw, i))) {
               return nullptr;
           }
        }

        if (!Views.empty()) {
            AddError("Not all views have been dropped");
            return nullptr;
        }

        Statements.push_back(L(A("let"), A("world"), L(A("CommitAll!"),
            A("world"))));
        Statements.push_back(L(A("return"), A("world")));

        if (DqEngineEnabled) {
            Statements[DqEnginePgmPos] = L(A("let"), A("world"), L(A(TString(NYql::ConfigureName)), A("world"), configSource,
                QA("DqEngine"), QA(DqEngineForce ? "force" : "auto")));
        } else {
            Statements.erase(Statements.begin() + DqEnginePgmPos);
        }

        return VL(Statements.data(), Statements.size());
    }

    [[nodiscard]]
    bool ParseRawStmt(const RawStmt* value) {
        auto node = value->stmt;
        switch (NodeTag(node)) {
        case T_SelectStmt:
            return ParseSelectStmt(CAST_NODE(SelectStmt, node), false) != nullptr;
        case T_InsertStmt:
            return ParseInsertStmt(CAST_NODE(InsertStmt, node)) != nullptr;
        case T_ViewStmt:
            return ParseViewStmt(CAST_NODE(ViewStmt, node)) != nullptr;
        case T_DropStmt:
            return ParseDropStmt(CAST_NODE(DropStmt, node)) != nullptr;
        case T_VariableSetStmt:
            return ParseVariableSetStmt(CAST_NODE(VariableSetStmt, node)) != nullptr;
        default:
            NodeNotImplemented(value, node);
            return false;
        }
    }

    using TTraverseSelectStack = TStack<std::pair<const SelectStmt*, bool>>;
    using TTraverseNodeStack = TStack<std::pair<const Node*, bool>>;

    [[nodiscard]]
    TAstNode* ParseSelectStmt(const SelectStmt* value, bool inner) {
        CTE.emplace_back();
        Y_DEFER {
            CTE.pop_back();
        };

        if (value->withClause) {
            if (!ParseWithClause(CAST_NODE(WithClause, value->withClause))) {
                return nullptr;
            }
        }

        TTraverseSelectStack traverseSelectStack;
        traverseSelectStack.push({ value, false });

        TVector<const SelectStmt*> setItems;
        TVector<TAstNode*> setOpsNodes;

        while (!traverseSelectStack.empty()) {
            auto& top = traverseSelectStack.top();
            if (top.first->op == SETOP_NONE) {
                // leaf
                setItems.push_back(top.first);
                setOpsNodes.push_back(QA("push"));
                traverseSelectStack.pop();
            } else {
                if (!top.first->larg || !top.first->rarg) {
                    AddError("SelectStmt: expected larg and rarg");
                    return nullptr;
                }

                if (!top.second) {
                    traverseSelectStack.push({ top.first->rarg, false });
                    traverseSelectStack.push({ top.first->larg, false });
                    top.second = true;
                } else {
                    TString op;
                    switch (top.first->op) {
                    case SETOP_UNION:
                        op = "union"; break;
                    case SETOP_INTERSECT:
                        op = "intersect"; break;
                    case SETOP_EXCEPT:
                        op = "except"; break;
                    default:
                        AddError(TStringBuilder() << "SetOperation unsupported value: " << (int)top.first->op);
                        return nullptr;
                    }

                    if (top.first->all) {
                        op += "_all";
                    }

                    setOpsNodes.push_back(QA(op));
                    traverseSelectStack.pop();
                }
            }
        }


        TAstNode* sort = nullptr;
        if (ListLength(value->sortClause) > 0) {
            TVector<TAstNode*> sortItems;
            for (int i = 0; i < ListLength(value->sortClause); ++i) {
                auto node = ListNodeNth(value->sortClause, i);
                if (NodeTag(node) != T_SortBy) {
                    NodeNotImplemented(value, node);
                    return nullptr;
                }

                auto sort = ParseSortBy(CAST_NODE_EXT(PG_SortBy, T_SortBy, node), setItems.size() == 1);
                if (!sort) {
                    return nullptr;
                }

                sortItems.push_back(sort);
            }

            sort = QVL(sortItems.data(), sortItems.size());
        }

        TVector<TAstNode*> setItemNodes;
        for (const auto& x : setItems) {
            bool hasDistinctAll = false;
            TVector<TAstNode*> distinctOnItems;
            if (x->distinctClause) {
                if (linitial(x->distinctClause) == NULL) {
                    hasDistinctAll = true;
                } else {
                    for (int i = 0; i < ListLength(x->distinctClause); ++i) {
                        auto node = ListNodeNth(x->distinctClause, i);
                        TExprSettings settings;
                        settings.AllowColumns = true;
                        settings.Scope = "DISTINCT ON";
                        auto expr = ParseExpr(node, settings);
                        if (!expr) {
                            return nullptr;
                        }

                        auto lambda = L(A("lambda"), QL(), expr);
                        distinctOnItems.push_back(L(A("PgGroup"), L(A("Void")), lambda));
                    }
                }
            }

            if (x->intoClause) {
                AddError("SelectStmt: not supported intoClause");
                return nullptr;
            }

            TVector<TAstNode*> fromList;
            TVector<TAstNode*> joinOps;
            for (int i = 0; i < ListLength(x->fromClause); ++i) {
                auto node = ListNodeNth(x->fromClause, i);
                if (NodeTag(node) != T_JoinExpr) {
                    auto p = ParseFromClause(node);
                    if (!p.Source) {
                        return nullptr;
                    }

                    AddFrom(p, fromList);
                    joinOps.push_back(QL());
                } else {
                    TTraverseNodeStack traverseNodeStack;
                    traverseNodeStack.push({ node, false });
                    TVector<TAstNode*> oneJoinGroup;

                    while (!traverseNodeStack.empty()) {
                        auto& top = traverseNodeStack.top();
                        if (NodeTag(top.first) != T_JoinExpr) {
                            // leaf
                            auto p = ParseFromClause(top.first);
                            if (!p.Source) {
                                return nullptr;
                            }

                            AddFrom(p, fromList);
                            traverseNodeStack.pop();
                        } else {
                            auto join = CAST_NODE(JoinExpr, top.first);
                            if (!join->larg || !join->rarg) {
                                AddError("JoinExpr: expected larg and rarg");
                                return nullptr;
                            }

                            if (join->alias) {
                                AddError("JoinExpr: unsupported alias");
                                return nullptr;
                            }

                            if (join->isNatural) {
                                AddError("JoinExpr: unsupported isNatural");
                                return nullptr;
                            }

                            if (ListLength(join->usingClause) > 0) {
                                AddError("JoinExpr: unsupported using");
                                return nullptr;
                            }

                            if (!top.second) {
                                traverseNodeStack.push({ join->rarg, false });
                                traverseNodeStack.push({ join->larg, false });
                                top.second = true;
                            } else {
                                TString op;
                                switch (join->jointype) {
                                case JOIN_INNER:
                                    op = join->quals ? "inner" : "cross"; break;
                                case JOIN_LEFT:
                                    op = "left"; break;
                                case JOIN_FULL:
                                    op = "full"; break;
                                case JOIN_RIGHT:
                                    op = "right"; break;
                                default:
                                    AddError(TStringBuilder() << "jointype unsupported value: " << (int)join->jointype);
                                    return nullptr;
                                }

                                if (op != "cross" && !join->quals) {
                                    AddError("join_expr: expected quals for non-cross join");
                                    return nullptr;
                                }

                                if (op == "cross") {
                                    oneJoinGroup.push_back(QL(QA(op)));
                                } else {
                                    TExprSettings settings;
                                    settings.AllowColumns = true;
                                    settings.Scope = "JOIN ON";
                                    auto quals = ParseExpr(join->quals, settings);
                                    if (!quals) {
                                        return nullptr;
                                    }

                                    auto lambda = L(A("lambda"), QL(), quals);
                                    oneJoinGroup.push_back(QL(QA(op), L(A("PgWhere"), L(A("Void")), lambda)));
                                }

                                traverseNodeStack.pop();
                            }
                        }
                    }

                    joinOps.push_back(QVL(oneJoinGroup.data(), oneJoinGroup.size()));
                }
            }

            TAstNode* whereFilter = nullptr;
            if (x->whereClause) {
                TExprSettings settings;
                settings.AllowColumns = true;
                settings.Scope = "WHERE";
                whereFilter = ParseExpr(x->whereClause, settings);
                if (!whereFilter) {
                    return nullptr;
                }
            }

            TAstNode* groupBy = nullptr;
            if (ListLength(x->groupClause) > 0) {
                TVector<TAstNode*> groupByItems;
                for (int i = 0; i < ListLength(x->groupClause); ++i) {
                    auto node = ListNodeNth(x->groupClause, i);
                    if (NodeTag(node) != T_ColumnRef) {
                        NodeNotImplemented(x, node);
                        return nullptr;
                    }

                    auto ref = ParseColumnRef(CAST_NODE(ColumnRef, node));
                    if (!ref) {
                        return nullptr;
                    }

                    auto lambda = L(A("lambda"), QL(), ref);
                    groupByItems.push_back(L(A("PgGroup"), L(A("Void")), lambda));
                }

                groupBy = QVL(groupByItems.data(), groupByItems.size());
            }

            TAstNode* having = nullptr;
            if (x->havingClause) {
                TExprSettings settings;
                settings.AllowColumns = true;
                settings.Scope = "HAVING";
                settings.AllowAggregates = true;
                having = ParseExpr(x->havingClause, settings);
                if (!having) {
                    return nullptr;
                }
            }

            TVector<TAstNode*> windowItems;
            if (ListLength(x->windowClause) > 0) {
                for (int i = 0; i < ListLength(x->windowClause); ++i) {
                    auto node = ListNodeNth(x->windowClause, i);
                    if (NodeTag(node) != T_WindowDef) {
                        NodeNotImplemented(x, node);
                        return nullptr;
                    }

                    auto win = ParseWindowDef(CAST_NODE(WindowDef, node));
                    if (!win) {
                        return nullptr;
                    }

                    windowItems.push_back(win);
                }
            }

            if (ListLength(x->valuesLists) && ListLength(x->fromClause)) {
                AddError("SelectStmt: values_lists isn't compatible to from_clause");
                return nullptr;
            }

            if (!ListLength(x->valuesLists) == !ListLength(x->targetList)) {
                AddError("SelectStmt: only one of values_lists and target_list should be specified");
                return nullptr;
            }

            if (x != value && ListLength(x->sortClause) > 0) {
                AddError("SelectStmt: sortClause should be used only on top");
                return nullptr;
            }

            if (x != value) {
                if (x->limitOption == LIMIT_OPTION_COUNT || x->limitOption == LIMIT_OPTION_DEFAULT) {
                    if (value->limitCount || value->limitOffset) {
                        AddError("SelectStmt: limit should be used only on top");
                        return nullptr;
                    }
                } else {
                    AddError(TStringBuilder() << "LimitOption unsupported value: " << (int)x->limitOption);
                    return nullptr;
                }
            }

            if (ListLength(x->lockingClause) > 0) {
                AddError("SelectStmt: not supported lockingClause");
                return nullptr;
            }

            TVector<TAstNode*> res;
            ui32 i = 0;
            for (int targetIndex = 0; targetIndex < ListLength(x->targetList); ++targetIndex) {
                auto node = ListNodeNth(x->targetList, targetIndex);
                if (NodeTag(node) != T_ResTarget) {
                    NodeNotImplemented(x, node);
                    return nullptr;
                }

                auto r = CAST_NODE(ResTarget, node);
                if (!r->val) {
                    AddError("SelectStmt: expected val");
                    return nullptr;
                }

                TExprSettings settings;
                settings.AllowColumns = true;
                settings.AllowAggregates = true;
                settings.AllowOver = true;
                settings.WindowItems = &windowItems;
                settings.Scope = "SELECT";
                auto x = ParseExpr(r->val, settings);
                if (!x) {
                    return nullptr;
                }

                bool isStar = false;
                if (NodeTag(r->val) == T_ColumnRef) {
                    auto ref = CAST_NODE(ColumnRef, r->val);
                    for (int fieldNo = 0; fieldNo < ListLength(ref->fields); ++fieldNo) {
                        if (NodeTag(ListNodeNth(ref->fields, fieldNo)) == T_A_Star) {
                            isStar = true;
                            break;
                        }
                    }
                }

                TString name;
                if (!isStar) {
                    name = r->name;
                    if (name.empty()) {
                        if (NodeTag(r->val) == T_ColumnRef) {
                            auto ref = CAST_NODE(ColumnRef, r->val);
                            auto field = ListNodeNth(ref->fields, ListLength(ref->fields) - 1);
                            if (NodeTag(field) == T_String) {
                                name = StrVal(field);
                            }
                        }
                    }

                    if (name.empty()) {
                        name = "column" + ToString(i++);
                    }
                }

                auto lambda = L(A("lambda"), QL(), x);
                auto columnName = QAX(name);
                res.push_back(L(A("PgResultItem"), columnName, L(A("Void")), lambda));
            }

            TVector<TAstNode*> val;
            TVector<TAstNode*> valNames;
            val.push_back(A("AsList"));

            for (int valueIndex = 0; valueIndex < ListLength(x->valuesLists); ++valueIndex) {
                TExprSettings settings;
                settings.AllowColumns = false;
                settings.Scope = "VALUES";

                auto node = ListNodeNth(x->valuesLists, valueIndex);
                if (NodeTag(node) != T_List) {
                    NodeNotImplemented(x, node);
                    return nullptr;
                }

                auto lst = CAST_NODE(List, node);
                TVector<TAstNode*> row;
                if (valueIndex == 0) {
                    for (int item = 0; item < ListLength(lst); ++item) {
                        valNames.push_back(QA("column" + ToString(i++)));
                    }
                } else {
                    if (ListLength(lst) != (int)valNames.size()) {
                        AddError("SelectStmt: VALUES lists must all be the same length");
                        return nullptr;
                    }
                }

                for (int item = 0; item < ListLength(lst); ++item) {
                    auto cell = ParseExpr(ListNodeNth(lst, item), settings);
                    if (!cell) {
                        return nullptr;
                    }

                    row.push_back(cell);
                }

                val.push_back(QVL(row.data(), row.size()));
            }

            TVector<TAstNode*> setItemOptions;
            if (ListLength(x->targetList) > 0) {
                setItemOptions.push_back(QL(QA("result"), QVL(res.data(), res.size())));
            } else {
                setItemOptions.push_back(QL(QA("values"), QVL(valNames.data(), valNames.size()), VL(val.data(), val.size())));
            }

            if (!fromList.empty()) {
                setItemOptions.push_back(QL(QA("from"), QVL(fromList.data(), fromList.size())));
                setItemOptions.push_back(QL(QA("join_ops"), QVL(joinOps.data(), joinOps.size())));
            }

            if (whereFilter) {
                auto lambda = L(A("lambda"), QL(), whereFilter);
                setItemOptions.push_back(QL(QA("where"), L(A("PgWhere"), L(A("Void")), lambda)));
            }

            if (groupBy) {
                setItemOptions.push_back(QL(QA("group_by"), groupBy));
            }

            if (windowItems.size()) {
                auto window = QVL(windowItems.data(), windowItems.size());
                setItemOptions.push_back(QL(QA("window"), window));
            }

            if (having) {
                auto lambda = L(A("lambda"), QL(), having);
                setItemOptions.push_back(QL(QA("having"), L(A("PgWhere"), L(A("Void")), lambda)));
            }

            if (hasDistinctAll) {
                setItemOptions.push_back(QL(QA("distinct_all")));
            } else if (!distinctOnItems.empty()) {
                auto distinctOn = QVL(distinctOnItems.data(), distinctOnItems.size());
                setItemOptions.push_back(QL(QA("distinct_on"), distinctOn));
            }

            if (setItems.size() == 1 && sort) {
                setItemOptions.push_back(QL(QA("sort"), sort));
            }

            auto setItem = L(A("PgSetItem"), QVL(setItemOptions.data(), setItemOptions.size()));
            setItemNodes.push_back(setItem);
        }

        if (value->intoClause) {
            AddError("SelectStmt: not supported intoClause");
            return nullptr;
        }

        if (ListLength(value->lockingClause) > 0) {
            AddError("SelectStmt: not supported lockingClause");
            return nullptr;
        }

        TAstNode* limit = nullptr;
        TAstNode* offset = nullptr;
        if (value->limitOption == LIMIT_OPTION_COUNT || value->limitOption == LIMIT_OPTION_DEFAULT) {
            if (value->limitCount) {
                TExprSettings settings;
                settings.AllowColumns = false;
                settings.Scope = "LIMIT";
                limit = ParseExpr(value->limitCount, settings);
                if (!limit) {
                    return nullptr;
                }
            }

            if (value->limitOffset) {
                TExprSettings settings;
                settings.AllowColumns = false;
                settings.Scope = "OFFSET";
                offset = ParseExpr(value->limitOffset, settings);
                if (!offset) {
                    return nullptr;
                }
            }
        } else {
            AddError(TStringBuilder() << "LimitOption unsupported value: " << (int)value->limitOption);
            return nullptr;
        }

        TVector<TAstNode*> selectOptions;

        selectOptions.push_back(QL(QA("set_items"), QVL(setItemNodes.data(), setItemNodes.size())));
        selectOptions.push_back(QL(QA("set_ops"), QVL(setOpsNodes.data(), setOpsNodes.size())));

        if (setItems.size() > 1 && sort) {
            selectOptions.push_back(QL(QA("sort"), sort));
        }

        if (limit) {
            selectOptions.push_back(QL(QA("limit"), limit));
        }

        if (offset) {
            selectOptions.push_back(QL(QA("offset"), offset));
        }

        auto output = L(A("PgSelect"), QVL(selectOptions.data(), selectOptions.size()));

        if (inner) {
            return output;
        }

        auto resOptions = QL(QL(QA("type")), QL(QA("autoref")));
        Statements.push_back(L(A("let"), A("output"), output));
        Statements.push_back(L(A("let"), A("result_sink"), L(A("DataSink"), QA(TString(NYql::ResultProviderName)))));
        Statements.push_back(L(A("let"), A("world"), L(A("Write!"),
            A("world"), A("result_sink"), L(A("Key")), A("output"), resOptions)));
        Statements.push_back(L(A("let"), A("world"), L(A("Commit!"),
            A("world"), A("result_sink"))));
        return Statements.back();
    }

    [[nodiscard]]
    bool ParseWithClause(const WithClause* value) {
        if (value->recursive) {
            AddError("WithClause: recursion is not supported");
            return false;
        }

        for (int i = 0; i < ListLength(value->ctes); ++i) {
            auto object = ListNodeNth(value->ctes, i);
            if (NodeTag(object) != T_CommonTableExpr) {
                NodeNotImplemented(value, object);
                return false;
            }

            if (!ParseCTE(CAST_NODE(CommonTableExpr, object))) {
                return false;
            }
        }

        return true;
    }

    [[nodiscard]]
    bool ParseCTE(const CommonTableExpr* value) {
        TView view;
        view.Name = value->ctename;

        for (int i = 0; i < ListLength(value->aliascolnames); ++i) {
            auto node = ListNodeNth(value->aliascolnames, i);
            if (NodeTag(node) != T_String) {
                NodeNotImplemented(value, node);
                return false;
            }

            view.ColNames.push_back(StrVal(node));
        }

        if (NodeTag(value->ctequery) != T_SelectStmt) {
            AddError("Expected Select statement as CTE query");
            return false;
        }

        view.Source = ParseSelectStmt(CAST_NODE(SelectStmt, value->ctequery), true);
        if (!view.Source) {
            return false;
        }

        auto& currentCTEs = CTE.back();
        if (currentCTEs.find(view.Name) != currentCTEs.end()) {
            AddError(TStringBuilder() << "CTE already exists: '" << view.Name << "'");
            return false;
        }

        currentCTEs[view.Name] = view;
        return true;
    }

    [[nodiscard]]
    TAstNode* ParseInsertStmt(const InsertStmt* value) {
        if (ListLength(value->cols) > 0) {
            AddError("InsertStmt: target columns are not supported");
            return nullptr;
        }

        if (!value->selectStmt) {
            AddError("InsertStmt: expected Select");
            return nullptr;
        }

        if (value->onConflictClause) {
            AddError("InsertStmt: not supported onConflictClause");
            return nullptr;
        }

        if (ListLength(value->returningList) > 0) {
            AddError("InsertStmt: not supported returningList");
            return nullptr;
        }

        if (value->withClause) {
            AddError("InsertStmt: not supported withClause");
            return nullptr;
        }

        auto insertDesc = ParseWriteRangeVar(value->relation);
        if (!insertDesc.Sink) {
            return nullptr;
        }

        auto select = ParseSelectStmt(CAST_NODE(SelectStmt, value->selectStmt), true);
        if (!select) {
            return nullptr;
        }

        auto writeOptions = QL(QL(QA("mode"), QA("append")));
        Statements.push_back(L(A("let"), A("world"), L(A("Write!"),
            A("world"), insertDesc.Sink, insertDesc.Key, select, writeOptions)));

        return Statements.back();
    }

    [[nodiscard]]
    TAstNode* ParseViewStmt(const ViewStmt* value) {
        if (ListLength(value->options) > 0) {
            AddError("Create view: not supported options");
            return nullptr;
        }

        TView view;
        if (StrLength(value->view->catalogname) > 0) {
            AddError("catalogname is not supported");
            return nullptr;
        }

        if (StrLength(value->view->schemaname) > 0) {
            AddError("schemaname is not supported");
            return nullptr;
        }

        if (StrLength(value->view->relname) == 0) {
            AddError("relname should be specified");
            return nullptr;
        }

        view.Name = value->view->relname;
        if (value->view->alias) {
            AddError("alias is not supported");
            return nullptr;
        }

        if (ListLength(value->aliases) == 0) {
            AddError("expected at least one target column");
            return nullptr;
        }

        for (int i = 0; i < ListLength(value->aliases); ++i) {
            auto node = ListNodeNth(value->aliases, i);
            if (NodeTag(node) != T_String) {
                NodeNotImplemented(value, node);
                return nullptr;
            }

            view.ColNames.push_back(StrVal(node));
        }

        if (value->withCheckOption != NO_CHECK_OPTION) {
            AddError("Create view: not supported options");
            return nullptr;
        }


        view.Source = ParseSelectStmt(CAST_NODE(SelectStmt, value->query), true);
        if (!view.Source) {
            return nullptr;
        }

        auto it = Views.find(view.Name);
        if (it != Views.end() && !value->replace) {
            AddError(TStringBuilder() << "View already exists: '" << view.Name << "'");
            return nullptr;
        }

        Views[view.Name] = view;
        return Statements.back();
    }

    [[nodiscard]]
    TAstNode* ParseDropStmt(const DropStmt* value) {
        if (value->removeType != OBJECT_VIEW) {
            AddError("Not supported object type for DROP");
            return nullptr;
        }

        // behavior and concurrent don't matter here
        for (int i = 0; i < ListLength(value->objects); ++i) {
            auto object = ListNodeNth(value->objects, i);
            if (NodeTag(object) != T_List) {
                NodeNotImplemented(value, object);
                return nullptr;
            }

            auto lst = CAST_NODE(List, object);
            if (ListLength(lst) != 1) {
                AddError("Expected view name");
                return nullptr;
            }

            auto nameNode = ListNodeNth(lst, 0);
            if (NodeTag(nameNode) != T_String) {
                NodeNotImplemented(value, nameNode);
                return nullptr;
            }

            auto name = StrVal(nameNode);
            auto it = Views.find(name);
            if (!value->missing_ok && it == Views.end()) {
                AddError(TStringBuilder() << "View not found: '" << name << "'");
                return nullptr;
            }

            if (it != Views.end()) {
                Views.erase(it);
            }
        }

        return Statements.back();
    }

    [[nodiscard]]
    TAstNode* ParseVariableSetStmt(const VariableSetStmt* value) {
        if (value->kind != VAR_SET_VALUE) {
            AddError(TStringBuilder() << "VariableSetStmt, not supported kind: " << (int)value->kind);
            return nullptr;
        }

        auto name = to_lower(TString(value->name));
        if (name == "dqengine") {
            if (ListLength(value->args) != 1) {
                AddError(TStringBuilder() << "VariableSetStmt, expected 1 arg, but got: " << ListLength(value->args));
                return nullptr;
            }

            auto arg = ListNodeNth(value->args, 0);
            if (NodeTag(arg) == T_A_Const && (NodeTag(CAST_NODE(A_Const, arg)->val) == T_String)) {
                auto rawStr = StrVal(CAST_NODE(A_Const, arg)->val);
                auto str = to_lower(TString(rawStr));
                if (str == "auto") {
                    DqEngineEnabled = true;
                    DqEngineForce = false;
                } else if (str == "force") {
                    DqEngineEnabled = true;
                    DqEngineForce = true;
                } else if (str == "disable") {
                    DqEngineEnabled = false;
                    DqEngineForce = false;
                } else {
                    AddError(TStringBuilder() << "VariableSetStmt, not supported DqEngine option value: " << rawStr);
                    return nullptr;
                }
            } else {
                AddError(TStringBuilder() << "VariableSetStmt, expected string literal for " << value->name << " option");
                return nullptr;
            }
        } else if (name.StartsWith("dq.") || name.StartsWith("yt.")) {
            if (ListLength(value->args) != 1) {
                AddError(TStringBuilder() << "VariableSetStmt, expected 1 arg, but got: " << ListLength(value->args));
                return nullptr;
            }

            auto arg = ListNodeNth(value->args, 0);
            if (NodeTag(arg) == T_A_Const && (NodeTag(CAST_NODE(A_Const, arg)->val) == T_String)) {
                auto dotPos = name.find('.');
                auto provider = name.substr(0, dotPos);
                auto providerSource = L(A("DataSource"), QA(TString(name.StartsWith("dq.") ? NYql::DqProviderName : NYql::YtProviderName)), QA("$all"));

                auto rawStr = StrVal(CAST_NODE(A_Const, arg)->val);

                Statements.push_back(L(A("let"), A("world"), L(A(TString(NYql::ConfigureName)), A("world"), providerSource,
                    QA("Attr"), QAX(name.substr(dotPos + 1)), QAX(rawStr))));
            } else {
                AddError(TStringBuilder() << "VariableSetStmt, expected string literal for " << value->name << " option");
                return nullptr;
            }
        } else if (name == "tablepathprefix") {
            if (ListLength(value->args) != 1) {
                AddError(TStringBuilder() << "VariableSetStmt, expected 1 arg, but got: " << ListLength(value->args));
                return nullptr;
            }

            auto arg = ListNodeNth(value->args, 0);
            if (NodeTag(arg) == T_A_Const && (NodeTag(CAST_NODE(A_Const, arg)->val) == T_String)) {
                auto rawStr = StrVal(CAST_NODE(A_Const, arg)->val);
                TablePathPrefix = rawStr;
            } else {
                AddError(TStringBuilder() << "VariableSetStmt, expected string literal for " << value->name << " option");
                return nullptr;
            }
        } else {
            AddError(TStringBuilder() << "VariableSetStmt, not supported name: " << value->name);
            return nullptr;
        }

        return Statements.back();
    }

    TFromDesc ParseFromClause(const Node* node) {
        switch (NodeTag(node)) {
        case T_RangeVar:
            return ParseRangeVar(CAST_NODE(RangeVar, node));
        case T_RangeSubselect:
            return ParseRangeSubselect(CAST_NODE(RangeSubselect, node));
        case T_RangeFunction:
            return ParseRangeFunction(CAST_NODE(RangeFunction, node));
        default:
            NodeNotImplementedImpl<SelectStmt>(node);
            return {};
        }
    }

    void AddFrom(const TFromDesc& p, TVector<TAstNode*>& fromList) {
        auto aliasNode = QAX(p.Alias);
        TVector<TAstNode*> colNamesNodes;
        for (const auto& c : p.ColNames) {
            colNamesNodes.push_back(QAX(c));
        }

        auto colNamesTuple = QVL(colNamesNodes.data(), colNamesNodes.size());
        if (p.InjectRead) {
            auto label = "read" + ToString(ReadIndex);
            Statements.push_back(L(A("let"), A(label), p.Source));
            Statements.push_back(L(A("let"), A("world"), L(A("Left!"), A(label))));
            fromList.push_back(QL(L(A("Right!"), A(label)), aliasNode, colNamesTuple));
            ++ReadIndex;
        } else {
            fromList.push_back(QL(p.Source, aliasNode, colNamesTuple));
        }
    }

    bool ParseAlias(const Alias* alias, TString& res, TVector<TString>& colnames) {
        for (int i = 0; i < ListLength(alias->colnames); ++i) {
            auto node = ListNodeNth(alias->colnames, i);
            if (NodeTag(node) != T_String) {
                NodeNotImplemented(alias, node);
                return false;
            }

            colnames.push_back(StrVal(node));
        }

        res = alias->aliasname;
        return true;
    }

    TInsertDesc ParseWriteRangeVar(const RangeVar* value) {
        if (StrLength(value->catalogname) > 0) {
            AddError("catalogname is not supported");
            return {};
        }

        if (StrLength(value->schemaname) == 0) {
            AddError("schemaname should be specified");
            return {};
        }

        if (StrLength(value->relname) == 0) {
            AddError("relname should be specified");
            return {};
        }

        if (value->alias) {
            AddError("alias is not supported");
            return {};
        }

        auto p = Settings.ClusterMapping.FindPtr(value->schemaname);
        if (!p) {
            AddError(TStringBuilder() << "Unknown cluster: " << value->schemaname);
            return {};
        }

        auto sink = L(A("DataSink"), QAX(*p), QAX(value->schemaname));
        auto key = L(A("Key"), QL(QA("table"), L(A("String"), QAX(value->relname))));
        return { sink, key };
    }

    TFromDesc ParseRangeVar(const RangeVar* value) {
        if (StrLength(value->catalogname) > 0) {
            AddError("catalogname is not supported");
            return {};
        }

        if (StrLength(value->relname) == 0) {
            AddError("relname should be specified");
            return {};
        }

        const TView* view = nullptr;
        if (StrLength(value->schemaname) == 0) {
            for (auto rit = CTE.rbegin(); rit != CTE.rend(); ++rit) {
                auto cteIt = rit->find(value->relname);
                if (cteIt != rit->end()) {
                    view = &cteIt->second;
                    break;
                }
            }

            if (!view) {
                auto viewIt = Views.find(value->relname);
                if (viewIt != Views.end()) {
                    view = &viewIt->second;
                } else {
                    AddError(TStringBuilder() << "View or CTE not found: '" << value->relname << "'");
                    return {};
                }
            }
        }

        TString alias;
        TVector<TString> colnames;
        if (value->alias) {
            if (!ParseAlias(value->alias, alias, colnames)) {
                return {};
            }
        } else {
            alias = value->relname;
        }

        if (view) {
            return { view->Source, alias, colnames.empty() ? view->ColNames : colnames, false };
        }

        auto p = Settings.ClusterMapping.FindPtr(value->schemaname);
        if (!p) {
            AddError(TStringBuilder() << "Unknown cluster: " << value->schemaname);
            return {};
        }

        auto source = L(A("DataSource"), QAX(*p), QAX(value->schemaname));
        return { L(A("Read!"), A("world"), source, L(A("Key"),
            QL(QA("table"), L(A("String"), QAX(TablePathPrefix + value->relname)))),
            L(A("Void")),
            QL()), alias, colnames, true };
    }

    TFromDesc ParseRangeFunction(const RangeFunction* value) {
        if (value->lateral) {
            AddError("RangeFunction: unsupported lateral");
            return {};
        }

        if (value->ordinality) {
            AddError("RangeFunction: unsupported ordinality");
            return {};
        }

        if (value->is_rowsfrom) {
            AddError("RangeFunction: unsupported is_rowsfrom");
            return {};
        }

        if (ListLength(value->coldeflist) > 0) {
            AddError("RangeFunction: unsupported coldeflist");
            return {};
        }

        if (ListLength(value->functions) != 1) {
            AddError("RangeFunction: only one function is supported");
            return {};
        }

        TString alias;
        TVector<TString> colnames;
        if (!value->alias) {
            AddError("RangeFunction: expected alias");
            return {};
        }

        if (!ParseAlias(value->alias, alias, colnames)) {
            return {};
        }

        auto funcNode = ListNodeNth(value->functions, 0);
        if (NodeTag(funcNode) != T_List) {
            AddError("RangeFunction: expected pair");
            return {};
        }

        auto lst = CAST_NODE(List, funcNode);
        if (ListLength(lst) != 2) {
            AddError("RangeFunction: expected pair");
            return {};
        }
        
        TExprSettings settings;
        settings.AllowColumns = false;
        settings.AllowReturnSet = true;
        settings.Scope = "RANGE FUNCTION";
        auto func = ParseExpr(ListNodeNth(lst, 0), settings);
        if (!func) {
            return {};
        }

        return { func, alias, colnames, false };
    }

    TFromDesc ParseRangeSubselect(const RangeSubselect* value) {
        if (value->lateral) {
            AddError("RangeSubselect: unsupported lateral");
            return {};
        }

        if (!value->alias) {
            AddError("RangeSubselect: expected alias");
            return {};
        }

        TString alias;
        TVector<TString> colnames;
        if (!ParseAlias(value->alias, alias, colnames)) {
            return {};
        }

        if (!value->subquery) {
            AddError("RangeSubselect: expected subquery");
            return {};
        }

        if (NodeTag(value->subquery) != T_SelectStmt) {
            NodeNotImplemented(value, value->subquery);
            return {};
        }

        return { ParseSelectStmt(CAST_NODE(SelectStmt, value->subquery), true), alias, colnames, false };
    }

    TAstNode* ParseNullTestExpr(const NullTest* value, const TExprSettings& settings) {
        if (value->argisrow) {
            AddError("NullTest: unsupported argisrow");
            return nullptr;
        }
        auto arg = ParseExpr(Expr2Node(value->arg), settings);
        if (!arg) {
            return nullptr;
        }
        auto result = L(A("Exists"), arg);
        if (value->nulltesttype == IS_NULL) {
            result = L(A("Not"), result);
        }
        return L(A("ToPg"), result);
    }

    struct TCaseBranch {
        TAstNode* Pred;
        TAstNode* Value;
    };

    TCaseBranch ReduceCaseBranches(std::vector<TCaseBranch>::const_iterator begin, std::vector<TCaseBranch>::const_iterator end) {
        Y_ENSURE(begin < end);
        const size_t branchCount = end - begin;
        if (branchCount == 1) {
            return *begin;
        }

        auto mid = begin + branchCount / 2;
        auto left = ReduceCaseBranches(begin, mid);
        auto right = ReduceCaseBranches(mid, end);

        TVector<TAstNode*> preds;
        preds.reserve(branchCount + 1);
        preds.push_back(A("Or"));
        for (auto it = begin; it != end; ++it) {
            preds.push_back(it->Pred);
        }

        TCaseBranch result;
        result.Pred = VL(&preds[0], preds.size());
        result.Value = L(A("If"), left.Pred, left.Value, right.Value);
        return result;

    }

    TAstNode* ParseCaseExpr(const CaseExpr* value, const TExprSettings& settings) {
        TAstNode* testExpr = nullptr;
        if (value->arg) {
            testExpr = ParseExpr(Expr2Node(value->arg), settings);
            if (!testExpr) {
                return nullptr;
            }
        }
        std::vector<TCaseBranch> branches;
        for (int i = 0; i < ListLength(value->args); ++i) {
            auto node = ListNodeNth(value->args, i);
            auto whenNode = CAST_NODE(CaseWhen, node);
            auto whenExpr = ParseExpr(Expr2Node(whenNode->expr), settings);
            if (!whenExpr) {
                return nullptr;
            }
            if (testExpr) {
                whenExpr = L(A("PgOp"), QA("="), testExpr, whenExpr);
            }

            whenExpr = L(A("Coalesce"),
                L(A("FromPg"), whenExpr),
                L(A("Bool"), QA("false"))
            );

            auto whenResult = ParseExpr(Expr2Node(whenNode->result), settings);
            if (!whenResult) {
                return nullptr;
            }
            branches.emplace_back(TCaseBranch{ .Pred = whenExpr,.Value = whenResult });
        }
        TAstNode* defaultResult = nullptr;
        if (value->defresult) {
            defaultResult = ParseExpr(Expr2Node(value->defresult), settings);
            if (!defaultResult) {
                return nullptr;
            }
        } else {
            defaultResult = L(A("Null"));
        }
        auto final = ReduceCaseBranches(branches.begin(), branches.end());
        return L(A("If"), final.Pred, final.Value, defaultResult);
    }

    TAstNode* ParseExpr(const Node* node, const TExprSettings& settings) {
        switch (NodeTag(node)) {
        case T_A_Const: {
            return ParseAConst(CAST_NODE(A_Const, node));
        }
        case T_A_Expr: {
            return ParseAExpr(CAST_NODE(A_Expr, node), settings);
        }
        case T_CaseExpr: {
            return ParseCaseExpr(CAST_NODE(CaseExpr, node), settings);
        }
        case T_ColumnRef: {
            if (!settings.AllowColumns) {
                AddError(TStringBuilder() << "Columns are not allowed in: " << settings.Scope);
                return nullptr;
            }

            return ParseColumnRef(CAST_NODE(ColumnRef, node));
        }
        case T_TypeCast: {
            return ParseTypeCast(CAST_NODE(TypeCast, node), settings);
        }
        case T_BoolExpr: {
            return ParseBoolExpr(CAST_NODE(BoolExpr, node), settings);
        }
        case T_NullTest: {
            return ParseNullTestExpr(CAST_NODE(NullTest, node), settings);
        }
        case T_FuncCall: {
            return ParseFuncCall(CAST_NODE(FuncCall, node), settings);
        }
        case T_A_ArrayExpr: {
            return ParseAArrayExpr(CAST_NODE(A_ArrayExpr, node), settings);
        }
        case T_SubLink: {
            return ParseSubLinkExpr(CAST_NODE(SubLink, node), settings);
        }
        case T_CoalesceExpr: {
            return ParseCoalesceExpr(CAST_NODE(CoalesceExpr, node), settings);
        }
        default:
            NodeNotImplemented(node);
            return nullptr;
        }
    }

    TAstNode* ParseAConst(const A_Const* value) {
        const auto& val = value->val;
        switch (NodeTag(val)) {
        case T_Integer: {
            return L(A("PgConst"), QA(ToString(IntVal(val))), L(A("PgType"), QA("int4")));
        }
        case T_Float: {
            return L(A("PgConst"), QA(ToString(StrFloatVal(val))), L(A("PgType"), QA("float8")));
        }
        case T_String: {
            return L(A("PgConst"), QAX(ToString(StrVal(val))), L(A("PgType"), QA("text")));
        }
        case T_Null: {
            return L(A("Null"));
        }
        default:
            ValueNotImplemented(value, val);
            return nullptr;
        }
    }

    TAstNode* ParseAArrayExpr(const A_ArrayExpr* value, const TExprSettings& settings) {
        TVector<TAstNode*> args;
        args.push_back(A("PgArray"));
        for (int i = 0; i < ListLength(value->elements); ++i) {
            auto elem = ParseExpr(ListNodeNth(value->elements, i), settings);
            if (!elem) {
                return nullptr;
            }

            args.push_back(elem);
        }

        return VL(args.data(), args.size());
    }

    TAstNode* ParseCoalesceExpr(const CoalesceExpr* value, const TExprSettings& settings) {
        TVector<TAstNode*> args;
        args.push_back(A("Coalesce"));
        for (int i = 0; i < ListLength(value->args); ++i) {
            auto elem = ParseExpr(ListNodeNth(value->args, i), settings);
            if (!elem) {
                return nullptr;
            }

            args.push_back(elem);
        }

        return VL(args.data(), args.size());
    }

    TAstNode* ParseSubLinkExpr(const SubLink* value, const TExprSettings& settings) {
        TString linkType;
        TString operName;
        switch (value->subLinkType) {
        case EXISTS_SUBLINK:
            linkType = "exists";
            break;
        case ALL_SUBLINK:
            linkType = "all";
            operName = "=";
            break;
        case ANY_SUBLINK:
            linkType = "any";
            operName = "=";
            break;
        case EXPR_SUBLINK:
            linkType = "expr";
            break;
        default:
            AddError(TStringBuilder() << "SublinkExpr: unsupported link type: " << (int)value->subLinkType);
            return nullptr;
        }

        if (ListLength(value->operName) > 1) {
            AddError("SubLink: unsuppoted opername");
            return nullptr;
        } else if (ListLength(value->operName) == 1) {
            auto nameNode = ListNodeNth(value->operName, 0);
            if (NodeTag(nameNode) != T_String) {
                NodeNotImplemented(value, nameNode);
                return nullptr;
            }

            operName = StrVal(nameNode);
        }

        TAstNode* rowTest;
        if (value->testexpr) {
            TExprSettings localSettings = settings;
            localSettings.Scope = "SUBLINK TEST";
            auto test = ParseExpr(value->testexpr, localSettings);
            if (!test) {
                return nullptr;
            }

            rowTest = L(A("lambda"), QL(A("value")), L(A("PgOp"), QAX(operName), test, A("value")));
        } else {
            rowTest = L(A("Void"));
        }

        auto select = ParseSelectStmt(CAST_NODE(SelectStmt, value->subselect), true);
        if (!select) {
            return nullptr;
        }

        return L(A("PgSubLink"), QA(linkType), L(A("Void")), L(A("Void")), rowTest, L(A("lambda"), QL(), select));
    }

    TAstNode* ParseFuncCall(const FuncCall* value, const TExprSettings& settings) {
        if (ListLength(value->agg_order) > 0) {
            AddError("FuncCall: unsupported agg_order");
            return nullptr;
        }

        if (value->agg_filter) {
            AddError("FuncCall: unsupported agg_filter");
            return nullptr;
        }

        if (value->agg_within_group) {
            AddError("FuncCall: unsupported agg_within_group");
            return nullptr;
        }

        if (value->func_variadic) {
            AddError("FuncCall: unsupported func_variadic");
            return nullptr;
        }

        TAstNode* window = nullptr;
        if (value->over) {
            if (!settings.AllowOver) {
                AddError(TStringBuilder() << "Over is not allowed in: " << settings.Scope);
                return nullptr;
            }

            if (StrLength(value->over->name)) {
                window = QAX(value->over->name);
            } else {
                auto index = settings.WindowItems->size();
                auto def = ParseWindowDef(value->over);
                if (!def) {
                    return nullptr;
                }

                window = L(A("PgAnonWindow"), QA(ToString(index)));
                settings.WindowItems->push_back(def);
            }
        }

        TVector<TString> names;
        for (int i = 0; i < ListLength(value->funcname); ++i) {
            auto x = ListNodeNth(value->funcname, i);
            if (NodeTag(x) != T_String) {
                NodeNotImplemented(value, x);
                return nullptr;
            }

            names.push_back(to_lower(TString(StrVal(x))));
        }

        if (names.empty()) {
            AddError("FuncCall: missing function name");
            return nullptr;
        }

        if (names.size() > 2) {
            AddError(TStringBuilder() << "FuncCall: too many name components:: " << names.size());
            return nullptr;
        }

        if (names.size() == 2 && names[0] != "pg_catalog") {
            AddError(TStringBuilder() << "FuncCall: expected pg_catalog, but got: " << names[0]);
            return nullptr;
        }

        auto name = names.back();
        const bool isAggregateFunc = NYql::NPg::HasAggregation(name);
        const bool hasReturnSet = NYql::NPg::HasReturnSetProc(name);

        if (isAggregateFunc && !settings.AllowAggregates) {
            AddError(TStringBuilder() << "Aggregate functions are not allowed in: " << settings.Scope);
            return nullptr;
        }

        if (hasReturnSet && !settings.AllowReturnSet) {
            AddError(TStringBuilder() << "Generator functions are not allowed in: " << settings.Scope);
            return nullptr;
        }

        TVector<TAstNode*> args;
        TString callable;
        if (window) {
            if (isAggregateFunc) {
                callable = "PgAggWindowCall";
            } else {
                callable = "PgWindowCall";
            }
        } else {
            if (isAggregateFunc) {
                callable = "PgAgg";
            } else {
                callable = "PgCall";
            }
        }

        args.push_back(A(callable));
        args.push_back(QAX(name));
        if (window) {
            args.push_back(window);
        }

        TVector<TAstNode*> callSettings;
        if (value->agg_distinct) {
            if (!isAggregateFunc) {
                AddError("FuncCall: agg_distinct must be set only for aggregate functions");
                return nullptr;
            }

            callSettings.push_back(QL(QA("distinct")));
        }

        args.push_back(QVL(callSettings.data(), callSettings.size()));
        if (value->agg_star) {
            if (name != "count") {
                AddError("FuncCall: * is expected only in count function");
                return nullptr;
            }
        } else {
            if (name == "count" && ListLength(value->args) == 0) {
                AddError("FuncCall: count(*) must be used to call a parameterless aggregate function");
                return nullptr;
            }

            bool hasError = false;
            for (int i = 0; i < ListLength(value->args); ++i) {
                auto x = ListNodeNth(value->args, i);
                auto arg = ParseExpr(x, settings);
                if (!arg) {
                    hasError = true;
                    continue;
                }

                args.push_back(arg);
            }

            if (hasError) {
                return nullptr;
            }
        }

        return VL(args.data(), args.size());
    }

    TAstNode* ParseTypeCast(const TypeCast* value, const TExprSettings& settings) {
        if (!value->arg) {
            AddError("Expected arg");
            return nullptr;
        }

        if (!value->typeName) {
            AddError("Expected type_name");
            return nullptr;
        }

        auto arg = value->arg;
        auto typeName = value->typeName;
        auto supportedTypeName = typeName->typeOid == 0 &&
            !typeName->setof &&
            !typeName->pct_type &&
            (ListLength(typeName->names) == 2 &&
                NodeTag(ListNodeNth(typeName->names, 0)) == T_String &&
                !StrCompare(StrVal(ListNodeNth(typeName->names, 0)), "pg_catalog") || ListLength(typeName->names) == 1) &&
            NodeTag(ListNodeNth(typeName->names, ListLength(typeName->names) - 1)) == T_String;

        if (NodeTag(arg) == T_A_Const &&
            (NodeTag(CAST_NODE(A_Const, arg)->val) == T_String ||
            NodeTag(CAST_NODE(A_Const, arg)->val) == T_Null) &&
            supportedTypeName &&
            typeName->typemod == -1 &&
            ListLength(typeName->typmods) == 0 &&
            ListLength(typeName->arrayBounds) == 0) {
            TStringBuf targetType = StrVal(ListNodeNth(typeName->names, ListLength(typeName->names) - 1));
            if (NodeTag(CAST_NODE(A_Const, arg)->val) == T_String && targetType == "bool") {
                auto str = StrVal(CAST_NODE(A_Const, arg)->val);
                return L(A("PgConst"), QAX(str), L(A("PgType"), QA("bool")));
            }
        }

        if (supportedTypeName) {
            TStringBuf targetType = StrVal(ListNodeNth(typeName->names, ListLength(typeName->names) - 1));
            auto input = ParseExpr(arg, settings);
            if (!input) {
                return nullptr;
            }

            auto finalType = TString(targetType);
            if (ListLength(typeName->arrayBounds) && !finalType.StartsWith('_')) {
                finalType = "_" + finalType;
            }

            if (!NPg::HasType(finalType)) {
                AddError(TStringBuilder() << "Unknown type: " << finalType);
                return nullptr;
            }

            if (ListLength(typeName->typmods) == 0 && typeName->typemod == -1) {
                return L(A("PgCast"), input, L(A("PgType"), QAX(finalType)));
            } else {
                const auto& typeDesc = NPg::LookupType(finalType);
                if (!typeDesc.TypeModInFuncId) {
                    AddError(TStringBuilder() << "Type " << finalType << " doesn't support modifiers");
                    return nullptr;
                }

                const auto& procDesc = NPg::LookupProc(typeDesc.TypeModInFuncId);

                TAstNode* typeMod;
                if (typeName->typemod != -1) {
                    typeMod = L(A("PgConst"), QA(ToString(typeName->typemod)), L(A("PgType"), QA("int4")));
                } else {
                    TVector<TAstNode*> args;
                    args.push_back(A("PgArray"));
                    for (int i = 0; i < ListLength(typeName->typmods); ++i) {
                        auto typeMod = ListNodeNth(typeName->typmods, i);
                        if (NodeTag(typeMod) != T_A_Const) {
                            AddError("Expected T_A_Const as typmod");
                            return nullptr;
                        }

                        auto aConst = CAST_NODE(A_Const, typeMod);
                        TString s;
                        if (!ValueAsString(aConst->val, s)) {
                            AddError("Unsupported format of typmod");
                            return nullptr;
                        }

                        args.push_back(L(A("PgConst"), QAX(s), L(A("PgType"), QA("cstring"))));
                    }

                    typeMod = L(A("PgCall"), QA(procDesc.Name), QL(), VL(args.data(), args.size()));
                }

                return L(A("PgCast"), input, L(A("PgType"), QAX(finalType)), typeMod);
            }
        }

        AddError("Unsupported form of type cast");
        return nullptr;
    }

    TAstNode* ParseAndOrExpr(const BoolExpr* value, const TExprSettings& settings, const TString& pgOpName) {
        auto length = ListLength(value->args);
        if (length < 2) {
            AddError(TStringBuilder() << "Expected >1 args for " << pgOpName << " but have " << length << " args");
            return nullptr;
        }

        auto lhs = ParseExpr(ListNodeNth(value->args, 0), settings);
        if (!lhs) {
            return nullptr;
        }

        for (auto i = 1; i < length; ++i) {
            auto rhs = ParseExpr(ListNodeNth(value->args, i), settings);
            if (!rhs) {
                return nullptr;
            }
            lhs = L(A(pgOpName), lhs, rhs);
        }

        return lhs;
    }

    TAstNode* ParseBoolExpr(const BoolExpr* value, const TExprSettings& settings) {
        switch (value->boolop) {
        case AND_EXPR: {
            return ParseAndOrExpr(value, settings, "PgAnd");
        }
        case OR_EXPR: {
            return ParseAndOrExpr(value, settings, "PgOr");
        }
        case NOT_EXPR: {
            if (ListLength(value->args) != 1) {
                AddError("Expected 1 arg for NOT");
                return nullptr;
            }

            auto arg = ParseExpr(ListNodeNth(value->args, 0), settings);
            if (!arg) {
                return nullptr;
            }

            return L(A("PgNot"), arg);
        }
        default:
            AddError(TStringBuilder() << "BoolExprType unsupported value: " << (int)value->boolop);
            return nullptr;
        }
    }

    TAstNode* ParseWindowDef(const WindowDef* value) {
        auto name = QAX(value->name);
        auto refName = QAX(value->refname);
        TVector<TAstNode*> sortItems;
        for (int i = 0; i < ListLength(value->orderClause); ++i) {
            auto node = ListNodeNth(value->orderClause, i);
            if (NodeTag(node) != T_SortBy) {
                NodeNotImplemented(value, node);
                return nullptr;
            }

            auto sort = ParseSortBy(CAST_NODE_EXT(PG_SortBy, T_SortBy, node), false);
            if (!sort) {
                return nullptr;
            }

            sortItems.push_back(sort);
        }

        auto sort = QVL(sortItems.data(), sortItems.size());
        TVector<TAstNode*> groupByItems;
        for (int i = 0; i < ListLength(value->partitionClause); ++i) {
            auto node = ListNodeNth(value->partitionClause, i);
            if (NodeTag(node) != T_ColumnRef) {
                NodeNotImplemented(value, node);
                return nullptr;
            }

            auto ref = ParseColumnRef(CAST_NODE(ColumnRef, node));
            if (!ref) {
                return nullptr;
            }

            auto lambda = L(A("lambda"), QL(), ref);
            groupByItems.push_back(L(A("PgGroup"), L(A("Void")), lambda));
        }

        auto group = QVL(groupByItems.data(), groupByItems.size());
        TVector<TAstNode*> optionItems;
        if (value->frameOptions & FRAMEOPTION_NONDEFAULT) {
            TString exclude;
            if (value->frameOptions & FRAMEOPTION_EXCLUDE_CURRENT_ROW) {
                if (exclude) {
                    AddError("Wrong frame options");
                    return nullptr;
                }

                exclude = "c";
            }

            if (value->frameOptions & FRAMEOPTION_EXCLUDE_GROUP) {
                if (exclude) {
                    AddError("Wrong frame options");
                    return nullptr;
                }

                exclude = "cp";
            }

            if (value->frameOptions & FRAMEOPTION_EXCLUDE_TIES) {
                if (exclude) {
                    AddError("Wrong frame options");
                    return nullptr;
                }

                exclude = "p";
            }

            if (exclude) {
                optionItems.push_back(QL(QA("exclude"), QA(exclude)));
            }

            TString type;
            if (value->frameOptions & FRAMEOPTION_RANGE) {
                if (type) {
                    AddError("Wrong frame options");
                    return nullptr;
                }

                type = "range";
            }

            if (value->frameOptions & FRAMEOPTION_ROWS) {
                if (type) {
                    AddError("Wrong frame options");
                    return nullptr;
                }

                type = "rows";
            }

            if (value->frameOptions & FRAMEOPTION_GROUPS) {
                if (type) {
                    AddError("Wrong frame options");
                    return nullptr;
                }

                type = "groups";
            }

            if (!type) {
                AddError("Wrong frame options");
                return nullptr;
            }

            TString from;
            if (value->frameOptions & FRAMEOPTION_START_UNBOUNDED_PRECEDING) {
                if (from) {
                    AddError("Wrong frame options");
                    return nullptr;
                }

                from = "up";
            }

            if (value->frameOptions & FRAMEOPTION_START_OFFSET_PRECEDING) {
                if (from) {
                    AddError("Wrong frame options");
                    return nullptr;
                }

                from = "p";
                auto offset = ConvertFrameOffset(value->startOffset);
                if (!offset) {
                    return nullptr;
                }

                optionItems.push_back(QL(QA("from_value"), offset));
            }

            if (value->frameOptions & FRAMEOPTION_START_CURRENT_ROW) {
                if (from) {
                    AddError("Wrong frame options");
                    return nullptr;
                }

                from = "c";
            }

            if (value->frameOptions & FRAMEOPTION_START_OFFSET_FOLLOWING) {
                if (from) {
                    AddError("Wrong frame options");
                    return nullptr;
                }

                from = "f";
                auto offset = ConvertFrameOffset(value->startOffset);
                if (!offset) {
                    return nullptr;
                }

                optionItems.push_back(QL(QA("from_value"), offset));
            }

            if (value->frameOptions & FRAMEOPTION_START_UNBOUNDED_FOLLOWING) {
                AddError("Wrong frame options");
                return nullptr;
            }

            if (!from) {
                AddError("Wrong frame options");
                return nullptr;
            }

            TString to;
            if (value->frameOptions & FRAMEOPTION_END_UNBOUNDED_PRECEDING) {
                AddError("Wrong frame options");
                return nullptr;
            }

            if (value->frameOptions & FRAMEOPTION_END_OFFSET_PRECEDING) {
                if (to) {
                    AddError("Wrong frame options");
                    return nullptr;
                }

                to = "p";
                auto offset = ConvertFrameOffset(value->endOffset);
                if (!offset) {
                    return nullptr;
                }

                optionItems.push_back(QL(QA("to_value"), offset));
            }

            if (value->frameOptions & FRAMEOPTION_END_CURRENT_ROW) {
                if (to) {
                    AddError("Wrong frame options");
                    return nullptr;
                }

                to = "c";
            }

            if (value->frameOptions & FRAMEOPTION_END_OFFSET_FOLLOWING) {
                if (to) {
                    AddError("Wrong frame options");
                    return nullptr;
                }

                to = "f";
                auto offset = ConvertFrameOffset(value->endOffset);
                if (!offset) {
                    return nullptr;
                }

                optionItems.push_back(QL(QA("to_value"), offset));
            }

            if (value->frameOptions & FRAMEOPTION_END_UNBOUNDED_FOLLOWING) {
                if (to) {
                    AddError("Wrong frame options");
                    return nullptr;
                }

                to = "uf";
            }

            if (!to) {
                AddError("Wrong frame options");
                return nullptr;
            }

            optionItems.push_back(QL(QA("type"), QAX(type)));
            optionItems.push_back(QL(QA("from"), QAX(from)));
            optionItems.push_back(QL(QA("to"), QAX(to)));
        }

        auto options = QVL(optionItems.data(), optionItems.size());
        return L(A("PgWindow"), name, refName, group, sort, options);
    }

    TAstNode* ConvertFrameOffset(const Node* off) {
        if (NodeTag(off) == T_A_Const
            && NodeTag(CAST_NODE(A_Const, off)->val) == T_Integer) {
            return L(A("Int32"), QA(ToString(IntVal(CAST_NODE(A_Const, off)->val))));
        } else {
            TExprSettings settings;
            settings.AllowColumns = false;
            settings.Scope = "FRAME";
            auto offset = ParseExpr(off, settings);
            if (!offset) {
                return nullptr;
            }

            return L(A("EvaluateExpr"), L(A("Unwrap"), offset, L(A("String"), QA("Frame offset must be non-null"))));
        }
    }

    TAstNode* ParseSortBy(const PG_SortBy* value, bool allowAggregates) {
        bool asc = true;
        switch (value->sortby_dir) {
        case SORTBY_DEFAULT:
            break;
        case SORTBY_ASC:
            break;
        case SORTBY_DESC:
            asc = false;
            break;
        default:
            AddError(TStringBuilder() << "sortby_dir unsupported value: " << (int)value->sortby_dir);
            return nullptr;
        }

        if (value->sortby_nulls != SORTBY_NULLS_DEFAULT) {
            AddError(TStringBuilder() << "sortby_nulls unsupported value: " << (int)value->sortby_nulls);
            return nullptr;
        }

        if (ListLength(value->useOp) > 0) {
            AddError("Unsupported operators in sort_by");
            return nullptr;
        }

        TExprSettings settings;
        settings.AllowColumns = true;
        settings.Scope = "ORDER BY";
        settings.AllowAggregates = allowAggregates;
        auto expr = ParseExpr(value->node, settings);
        if (!expr) {
            return nullptr;
        }

        auto lambda = L(A("lambda"), QL(), expr);
        return L(A("PgSort"), L(A("Void")), lambda, QA(asc ? "asc" : "desc"));
    }

    TAstNode* ParseColumnRef(const ColumnRef* value) {
        if (ListLength(value->fields) == 0) {
            AddError("No fields");
            return nullptr;
        }

        if (ListLength(value->fields) > 2) {
            AddError("Too many fields");
            return nullptr;
        }

        bool isStar = false;
        TVector<TString> fields;
        for (int i = 0; i < ListLength(value->fields); ++i) {
            auto x = ListNodeNth(value->fields, i);
            if (isStar) {
                AddError("Star is already defined");
                return nullptr;
            }

            if (NodeTag(x) == T_String) {
                fields.push_back(StrVal(x));
            } else if (NodeTag(x) == T_A_Star) {
                isStar = true;
            } else {
                NodeNotImplemented(value, x);
                return nullptr;
            }
        }

        if (isStar) {
            if (fields.size() == 0) {
                return L(A("PgStar"));
            } else {
                return L(A("PgQualifiedStar"), QAX(fields[0]));
            }
        } else if (fields.size() == 1) {
            return L(A("PgColumnRef"), QAX(fields[0]));
        } else {
            return L(A("PgColumnRef"), QAX(fields[0]), QAX(fields[1]));
        }
    }

    TAstNode* ParseAExprOp(const A_Expr* value, const TExprSettings& settings) {
        if (ListLength(value->name) != 1) {
            AddError(TStringBuilder() << "Unsupported count of names: " << ListLength(value->name));
            return nullptr;
        }

        auto nameNode = ListNodeNth(value->name, 0);
        if (NodeTag(nameNode) != T_String) {
            NodeNotImplemented(value, nameNode);
            return nullptr;
        }

        auto op = StrVal(nameNode);
        if (!value->rexpr) {
            AddError("Missing operands");
            return nullptr;
        }

        if (!value->lexpr) {
            auto rhs = ParseExpr(value->rexpr, settings);
            if (!rhs) {
                return nullptr;
            }

            return L(A("PgOp"), QAX(op), rhs);
        }

        auto lhs = ParseExpr(value->lexpr, settings);
        auto rhs = ParseExpr(value->rexpr, settings);
        if (!lhs || !rhs) {
            return nullptr;
        }

        return L(A("PgOp"), QAX(op), lhs, rhs);
    }

    TAstNode* ParseAExprLike(const A_Expr* value, const TExprSettings& settings, bool insensitive) {
        if (ListLength(value->name) != 1) {
            AddError(TStringBuilder() << "Unsupported count of names: " << ListLength(value->name));
            return nullptr;
        }

        auto nameNode = ListNodeNth(value->name, 0);
        if (NodeTag(nameNode) != T_String) {
            NodeNotImplemented(value, nameNode);
            return nullptr;
        }

        auto op = TString(StrVal(nameNode));
        if (insensitive) {
            if (op != "~~*" && op != "!~~*") {
                AddError(TStringBuilder() << "Unsupported operation: " << op);
                return nullptr;
            }
        } else {
            if (op != "~~" && op != "!~~") {
                AddError(TStringBuilder() << "Unsupported operation: " << op);
                return nullptr;
            }
        }

        if (!value->lexpr || !value->rexpr) {
            AddError("Missing operands");
            return nullptr;
        }

        auto lhs = ParseExpr(value->lexpr, settings);
        auto rhs = ParseExpr(value->rexpr, settings);
        if (!lhs || !rhs) {
            return nullptr;
        }

        auto ret = L(A(insensitive ? "PgILike" : "PgLike"), lhs, rhs);
        if (op[0] == '!') {
            ret = L(A("PgNot"), ret);
        }

        return ret;
    }

    TAstNode* ParseAExprIn(const A_Expr* value, const TExprSettings& settings) {
        if (ListLength(value->name) != 1) {
            AddError(TStringBuilder() << "Unsupported count of names: " << ListLength(value->name));
            return nullptr;
        }

        auto nameNode = ListNodeNth(value->name, 0);
        if (NodeTag(nameNode) != T_String) {
            NodeNotImplemented(value, nameNode);
            return nullptr;
        }

        auto op = TString(StrVal(nameNode));
        if (op != "=" && op != "<>") {
            AddError(TStringBuilder() << "Unsupported operation: " << op);
            return nullptr;
        }

        if (!value->lexpr || !value->rexpr) {
            AddError("Missing operands");
            return nullptr;
        }

        auto lhs = ParseExpr(value->lexpr, settings);
        if (!lhs) {
            return nullptr;
        }

        if (NodeTag(value->rexpr) != T_List) {
            NodeNotImplemented(value, value->rexpr);
            return nullptr;
        }

        auto lst = CAST_NODE(List, value->rexpr);
        TVector<TAstNode*> listItems;
        listItems.push_back(A("AsList"));
        for (int item = 0; item < ListLength(lst); ++item) {
            auto cell = ParseExpr(ListNodeNth(lst, item), settings);
            if (!cell) {
                return nullptr;
            }

            listItems.push_back(cell);
        }

        auto ret = L(A("PgIn"), lhs, VL(listItems.data(), listItems.size()));
        if (op[0] == '<') {
            ret = L(A("PgNot"), ret);
        }

        return ret;
    }

    TAstNode* ParseAExprBetween(const A_Expr* value, const TExprSettings& settings) {
        if (!value->lexpr || !value->rexpr) {
            AddError("Missing operands");
            return nullptr;
        }

        if (NodeTag(value->rexpr) != T_List) {
            AddError(TStringBuilder() << "Expected T_List tag, but have " << NodeTag(value->rexpr));
            return nullptr;
        }

        const List* rexprList = CAST_NODE(List, value->rexpr);
        if (ListLength(rexprList) != 2) {
            AddError(TStringBuilder() << "Expected 2 args in BETWEEN range, but have " << ListLength(rexprList));
            return nullptr;
        }

        auto b = ListNodeNth(rexprList, 0);
        auto e = ListNodeNth(rexprList, 1);

        auto lhs = ParseExpr(value->lexpr, settings);
        auto rbhs = ParseExpr(b, settings);
        auto rehs = ParseExpr(e, settings);
        if (!lhs || !rbhs || !rehs) {
            return nullptr;
        }

        A_Expr_Kind kind = value->kind;
        bool inverse = false;
        if (kind == AEXPR_NOT_BETWEEN) {
            inverse = true;
            kind = AEXPR_BETWEEN;
        } else if (kind == AEXPR_NOT_BETWEEN_SYM) {
            inverse = true;
            kind = AEXPR_BETWEEN_SYM;
        }

        TAstNode* ret;
        switch (kind) {
        case AEXPR_BETWEEN:
        case AEXPR_BETWEEN_SYM:
            ret = L(A(kind == AEXPR_BETWEEN ? "PgBetween" : "PgBetweenSym"), lhs, rbhs, rehs);
            break;
        default:
            AddError(TStringBuilder() << "BETWEEN kind unsupported value: " << (int)value->kind);
            return nullptr;
        }

        if (inverse) {
            ret = L(A("PgNot"), ret);
        }

        return ret;
    }

    TAstNode* ParseAExpr(const A_Expr* value, const TExprSettings& settings) {
        switch (value->kind) {
        case AEXPR_OP:
            return ParseAExprOp(value, settings);
        case AEXPR_LIKE:
        case AEXPR_ILIKE:
            return ParseAExprLike(value, settings, value->kind == AEXPR_ILIKE);
        case AEXPR_IN:
            return ParseAExprIn(value, settings);
        case AEXPR_BETWEEN:
        case AEXPR_NOT_BETWEEN:
        case AEXPR_BETWEEN_SYM:
        case AEXPR_NOT_BETWEEN_SYM:
            return ParseAExprBetween(value, settings);
        default:
            AddError(TStringBuilder() << "A_Expr_Kind unsupported value: " << (int)value->kind);
            return nullptr;
        }

    }

    template <typename T>
    void NodeNotImplementedImpl(const Node* nodeptr) {
        TStringBuilder b;
        b << TypeName<T>() << ": ";
        b << "alternative is not implemented yet : " << NodeTag(nodeptr);
        AddError(b);
    }

    template <typename T>
    void NodeNotImplemented(const T* outer, const Node* nodeptr) {
        Y_UNUSED(outer);
        NodeNotImplementedImpl<T>(nodeptr);
    }

    template <typename T>
    void ValueNotImplementedImpl(const Value& value) {
        TStringBuilder b;
        b << TypeName<T>() << ": ";
        b << "alternative is not implemented yet : " << NodeTag(value);
        AddError(b);
    }

    template <typename T>
    void ValueNotImplemented(const T* outer, const Value& value) {
        Y_UNUSED(outer);
        ValueNotImplementedImpl<T>(value);
    }

    void NodeNotImplemented(const Node* nodeptr) {
        TStringBuilder b;
        b << "alternative is not implemented yet : " << NodeTag(nodeptr);
        AddError(b);
    }

    TAstNode* VL(TAstNode** nodes, ui32 size, TPosition pos = {}) {
        return TAstNode::NewList(pos, nodes, size, *AstParseResult.Pool);
    }

    TAstNode* QVL(TAstNode** nodes, ui32 size, TPosition pos = {}) {
        return Q(VL(nodes, size, pos), pos);
    }

    TAstNode* A(const TString& str, TPosition pos = {}, ui32 flags = 0) {
        return TAstNode::NewAtom(pos, str, *AstParseResult.Pool, flags);
    }

    TAstNode* AX(const TString& str, TPosition pos = {}) {
        return A(str, pos, TNodeFlags::ArbitraryContent);
    }

    TAstNode* Q(TAstNode* node, TPosition pos = {}) {
        return L(A("quote", pos), node, pos);
    }

    TAstNode* QA(const TString& str, TPosition pos = {}, ui32 flags = 0) {
        return Q(A(str, pos, flags), pos);
    }

    TAstNode* QAX(const TString& str, TPosition pos = {}) {
        return QA(str, pos, TNodeFlags::ArbitraryContent);
    }

    template <typename... TNodes>
    TAstNode* L(TNodes... nodes) {
        TLState state;
        LImpl(state, nodes...);
        return TAstNode::NewList(state.Position, state.Nodes.data(), state.Nodes.size(), *AstParseResult.Pool);
    }

    template <typename... TNodes>
    TAstNode* QL(TNodes... nodes) {
        return Q(L(nodes...));
    }

private:
    void AddError(const TString& value) {
        AstParseResult.Issues.AddIssue(TIssue(value));
    }

    struct TLState {
        TPosition Position;
        TVector<TAstNode*> Nodes;
    };

    template <typename... TNodes>
    void LImpl(TLState& state, TNodes... nodes);

    void LImpl(TLState& state) {
        Y_UNUSED(state);
    }

    void LImpl(TLState& state, TPosition pos) {
        state.Position = pos;
    }

    void LImpl(TLState& state, TAstNode* node) {
        state.Nodes.push_back(node);
    }

    template <typename T, typename... TNodes>
    void LImpl(TLState& state, T node, TNodes... nodes) {
        state.Nodes.push_back(node);
        LImpl(state, nodes...);
    }

private:
    TAstParseResult& AstParseResult;
    NSQLTranslation::TTranslationSettings Settings;
    bool DqEngineEnabled = false;
    bool DqEngineForce = false;
    TVector<TAstNode*> Statements;
    ui32 DqEnginePgmPos = 0;
    ui32 ReadIndex = 0;
    TViews Views;
    TVector<TViews> CTE;
    TString TablePathPrefix;
};

NYql::TAstParseResult PGToYql(const TString& query, const NSQLTranslation::TTranslationSettings& settings) {
    NYql::TAstParseResult result;
    TConverter converter(result, settings);
    NYql::PGParse(query, converter);
    return result;
}

}  // NSQLTranslationPG
