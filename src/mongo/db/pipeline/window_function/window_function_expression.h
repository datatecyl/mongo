/**
 *    Copyright (C) 2020-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_set_window_fields_gen.h"
#include "mongo/db/pipeline/window_function/window_bounds.h"
#include "mongo/db/pipeline/window_function/window_function.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/sort_pattern.h"

#define REGISTER_NON_REMOVABLE_WINDOW_FUNCTION(name, parser)                         \
    MONGO_INITIALIZER_GENERAL(                                                       \
        addToWindowFunctionMap_##name, ("default"), ("windowFunctionExpressionMap")) \
    (InitializerContext*) {                                                          \
        ::mongo::window_function::Expression::registerParser("$" #name, parser);     \
    }

#define REGISTER_REMOVABLE_WINDOW_FUNCTION(name, accumClass, wfClass)                              \
    MONGO_INITIALIZER_GENERAL(                                                                     \
        addToWindowFunctionMap_##name, ("default"), ("windowFunctionExpressionMap"))               \
    (InitializerContext*) {                                                                        \
        ::mongo::window_function::Expression::registerParser(                                      \
            "$" #name, ::mongo::window_function::ExpressionRemovable<accumClass, wfClass>::parse); \
    }
namespace mongo::window_function {
/**
 * A window-function expression describes how to compute a single output value in a
 * $setWindowFields stage. For example, in
 *
 *     {$setWindowFields: {
 *         output: {
 *             totalCost: {$sum: {input: "$price"}},
 *             numItems: {$count: {}},
 *         }
 *     }}
 *
 * the two window-function expressions are {$sum: {input: "$price"}} and {$count: {}}.
 *
 * Because this class is part of a syntax tree, it does not hold any execution state:
 * instead it lets you create new instances of a window-function state.
 */
class Expression : public RefCountable {
public:
    /**
     * Parses a single window-function expression. The BSONElement's key is the function name,
     * and the value is the spec: for example, the whole BSONElement might be '$sum: {input:
     * "$x"}'.
     *
     * 'sortBy' is from the sortBy argument of $setWindowFields. Some window functions require
     * a sort spec, or require a one-field sort spec; they use this argument to enforce those
     * requirements.
     *
     * If the window function accepts bounds, parse() parses them, just like other arguments
     * such as 'input' or 'default'. For window functions like $rank, which don't accept bounds,
     * parse() is responsible for throwing a parse error, just like other unexpected arguments.
     */
    static boost::intrusive_ptr<Expression> parse(BSONElement elem,
                                                  const boost::optional<SortPattern>& sortBy,
                                                  ExpressionContext* expCtx);

    /**
     * A Parser has the same signature as parse(). The BSONElement is the whole expression, such
     * as '$sum: {input: "$x"}', because some parsers need to switch on the function name.
     */
    using Parser = std::function<decltype(parse)>;
    static void registerParser(std::string functionName, Parser parser);

    Expression(ExpressionContext* expCtx,
               std::string accumulatorName,
               boost::intrusive_ptr<::mongo::Expression> input,
               WindowBounds bounds)
        : _expCtx(expCtx),
          _accumulatorName(accumulatorName),
          _input(std::move(input)),
          _bounds(std::move(bounds)) {}

    std::string getOpName() const {
        return _accumulatorName;
    }

    WindowBounds bounds() const {
        return _bounds;
    }

    boost::intrusive_ptr<::mongo::Expression> input() const {
        return _input;
    }

    virtual boost::intrusive_ptr<AccumulatorState> buildAccumulatorOnly() const = 0;

    virtual std::unique_ptr<WindowFunctionState> buildRemovable() const = 0;

    virtual Value serialize(boost::optional<ExplainOptions::Verbosity> explain) const {
        MutableDocument args;

        args["input"] = _input->serialize(static_cast<bool>(explain));
        _bounds.serialize(args);

        return Value{Document{
            {_accumulatorName, args.freezeToValue()},
        }};
    }


protected:
    /*
     * Used during parsing to reject extra arguments in window function specifications.
     */
    static void checkAllowedFields(BSONElement elem) {
        static const StringSet allowedFields = {"input", "documents", "range", "unit"};
        for (auto&& arg : elem.embeddedObject()) {
            uassert(ErrorCodes::FailedToParse,
                    str::stream() << "Window function found an unknown argument: "
                                  << arg.fieldNameStringData(),
                    allowedFields.find(arg.fieldNameStringData()) != allowedFields.end());
        }
    }
    ExpressionContext* _expCtx;
    std::string _accumulatorName;
    boost::intrusive_ptr<::mongo::Expression> _input;
    WindowBounds _bounds;

private:
    static StringMap<Parser> parserMap;
};

template <typename NonRemovableType>
class ExpressionFromAccumulator : public Expression {
public:
    static boost::intrusive_ptr<Expression> parse(BSONElement elem,
                                                  const boost::optional<SortPattern>& sortBy,
                                                  ExpressionContext* expCtx) {
        // 'elem' is something like '$sum: {input: E, ...}'
        std::string accumulatorName = elem.fieldName();
        boost::intrusive_ptr<::mongo::Expression> input = ::mongo::Expression::parseOperand(
            expCtx, elem.embeddedObject()["input"], expCtx->variablesParseState);
        auto bounds = WindowBounds::parse(elem.Obj(), sortBy, expCtx);
        // Reject extra arguments.
        checkAllowedFields(elem);
        return make_intrusive<ExpressionFromAccumulator<NonRemovableType>>(
            expCtx, std::move(accumulatorName), std::move(input), std::move(bounds));
    }

    boost::intrusive_ptr<AccumulatorState> buildAccumulatorOnly() const final {
        return NonRemovableType::create(_expCtx);
    }
    std::unique_ptr<WindowFunctionState> buildRemovable() const final {
        uasserted(5461500,
                  str::stream() << "Window function " << _accumulatorName
                                << " is not supported with a removable window");
    }

    ExpressionFromAccumulator(ExpressionContext* expCtx,
                              std::string accumulatorName,
                              boost::intrusive_ptr<::mongo::Expression> input,
                              WindowBounds bounds)
        : Expression(expCtx, std::move(accumulatorName), std::move(input), std::move(bounds)) {}
};

template <typename NonRemovableType, typename RemovableType>
class ExpressionRemovable : public Expression {
public:
    static boost::intrusive_ptr<Expression> parse(BSONElement elem,
                                                  const boost::optional<SortPattern>& sortBy,
                                                  ExpressionContext* expCtx) {
        // 'elem' is something like '$sum: {input: E, ...}'
        std::string accumulatorName = elem.fieldName();
        boost::intrusive_ptr<::mongo::Expression> input = ::mongo::Expression::parseOperand(
            expCtx, elem.embeddedObject()["input"], expCtx->variablesParseState);
        auto bounds = WindowBounds::parse(elem.Obj(), sortBy, expCtx);
        // Reject extra arguments.
        checkAllowedFields(elem);
        return make_intrusive<ExpressionRemovable<NonRemovableType, RemovableType>>(
            expCtx, std::move(accumulatorName), std::move(input), std::move(bounds));
    }

    ExpressionRemovable(ExpressionContext* expCtx,
                        std::string accumulatorName,
                        boost::intrusive_ptr<::mongo::Expression> input,
                        WindowBounds bounds)
        : Expression(expCtx, std::move(accumulatorName), std::move(input), std::move(bounds)) {}

    boost::intrusive_ptr<AccumulatorState> buildAccumulatorOnly() const final {
        return NonRemovableType::create(_expCtx);
    }

    std::unique_ptr<WindowFunctionState> buildRemovable() const final {
        return RemovableType::create(_expCtx);
    }
};

}  // namespace mongo::window_function
