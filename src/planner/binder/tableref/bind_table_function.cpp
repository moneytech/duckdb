#include "duckdb/catalog/catalog.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression_binder/constant_binder.hpp"
#include "duckdb/planner/tableref/bound_table_function.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/common/algorithm.hpp"

using namespace duckdb;
using namespace std;

unique_ptr<BoundTableRef> Binder::Bind(TableFunctionRef &ref) {
	auto bind_index = GenerateTableIndex();

	assert(ref.function->type == ExpressionType::FUNCTION);
	auto fexpr = (FunctionExpression *)ref.function.get();

	// evalate the input parameters to the function
	vector<SQLType> arguments;
	vector<Value> parameters;
	for (auto &child : fexpr->children) {
		ConstantBinder binder(*this, context, "TABLE FUNCTION parameter");
		SQLType sql_type;
		auto expr = binder.Bind(child, &sql_type);
		auto constant = ExpressionExecutor::EvaluateScalar(*expr);
		constant.SetSQLType(sql_type);

		arguments.push_back(sql_type);
		parameters.push_back(move(constant));
	}
	// fetch the function from the catalog
	auto function =
	    Catalog::GetCatalog(context).GetEntry<TableFunctionCatalogEntry>(context, fexpr->schema, fexpr->function_name);

	// select the function based on the input parameters
	idx_t best_function_idx = Function::BindFunction(function->name, function->functions, arguments);
	auto &table_function = function->functions[best_function_idx];

	// cast the parameters to the type of the function
	auto result = make_unique<BoundTableFunction>(table_function, bind_index);
	for(idx_t i = 0; i < arguments.size(); i++) {
		if (table_function.arguments[i] == SQLType::ANY) {
			result->parameters.push_back(move(parameters[i]));
		} else {
			result->parameters.push_back(parameters[i].CastAs(arguments[i], table_function.arguments[i]));
		}
	}

	// perform the binding
	result->bind_data = table_function.bind(context, result->parameters, result->return_types, result->names);
	assert(result->return_types.size() == result->names.size());
	assert(result->return_types.size() > 0);
	vector<string> names;
	// first push any column name aliases
	for(idx_t i = 0; i < min<idx_t>(ref.column_name_alias.size(), result->names.size()); i++) {
		names.push_back(ref.column_name_alias[i]);
	}
	// then fill up the remainder with the given result names
	for(idx_t i = names.size(); i < result->names.size(); i++) {
		names.push_back(result->names[i]);
	}
	// now add the table function to the bind context so its columns can be bound
	bind_context.AddGenericBinding(bind_index, ref.alias.empty() ? fexpr->function_name : ref.alias, names,
	                               result->return_types);

	return move(result);
}
