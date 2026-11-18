// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <format>
#include <ranges>

#include <ast/decl/enum_declaration.hpp>
#include <ast/stmt/do_while_statement.hpp>
#include <ast/stmt/for_statement.hpp>
#include <ast/stmt/statements.hpp>
#include <ast/stmt/while_statement.hpp>
#include <semantic/checker.hpp>
#include <token/token.hpp>

namespace cloth::semantic {

BodyChecker::BodyChecker (Analyzer &analyzer, const Symbol &context) :
        analyzer(analyzer),
        context(context),
        owner(analyzer.declarations.at(&context).owner),
        returns(analyzer.declarations.at(&context).returns) {
	while (owner && owner->kind != SymbolKind::Type && owner->kind != SymbolKind::State) {
		owner = analyzer.declarations.at(owner).owner;
	}
}

const Binding *BodyChecker::local (const std::string &name) const {
	for (const auto &scope : std::views::reverse(scopes)) {
		const auto binding = scope.find(name);
		if (binding != scope.end()) {
			return &binding->second;
		}
	}
	return nullptr;
}

void BodyChecker::remember (const Token &use, const Token &declaration) const {
	if (record) {
		analyzer.program.references[&use] = &declaration;
	}
}

void BodyChecker::bind (const Token &name, ResolvedType type, const bool mutableBinding, const bool deferred) {
	const auto [fst, snd] = scopes.back().emplace(
	        name.get_lexeme(), Binding{ .type = std::move(type), .name = &name, .mutableBinding = mutableBinding, .deferred = deferred });
	if (!snd) {
		fail(Code::DUPLICATE_BINDING, name.get_span(), "Duplicate binding '" + name.get_lexeme() + "'", fst->second.name);
	}
	if (record) analyzer.program.localTypes[&name] = fst->second.type;
}

ResolvedType BodyChecker::selfType () const {
	if (!owner) {
		fail(Code::RECEIVER_OUTSIDE_METHOD, context.name.get_span(), "No enclosing receiver");
	}
	if (owner->kind == SymbolKind::State) {
		const DeclarationInfo &state = analyzer.declarations.at(owner);
		return state.owner ? analyzer.ownerType(*state.owner) : state.parents.front();
	}
	return analyzer.ownerType(*owner);
}

void BodyChecker::require (const ResultTypes &actual, const ResultTypes &expected, const TokenSpan &span, const Code code) const {
	if (actual.size() != expected.size()) {
		fail(code == Code::TYPE_MISMATCH ? Code::RESULT_ARITY : code, span,
		     std::format("Expected {} result(s), found {}", expected.size(), actual.size()));
	}
	for (std::size_t i = 0; i < actual.size(); ++i) {
		if (!analyzer.accepts(expected[i], actual[i])) {
			fail(code, span, "Expected " + expected[i].spelling() + ", found " + actual[i].spelling());
		}
	}
}

void BodyChecker::expect (const Expression &node, const ResolvedType &type) {
	const ResolvedType actual = scalar(node, &type);
	require({ actual }, { type }, node.get_span());
}

void BodyChecker::checkBody (const FunctionBody &body) {
	if (const BlockBody *block = std::get_if<BlockBody>(&body.value)) {
		bool fallsThrough = true;
		for (const StatementPtr &stmt : block->statements) {
			const bool next = statement(*stmt);
			fallsThrough &= next;
		}
		if (fallsThrough && !returns.empty()) {
			fail(Code::MISSING_RETURN, block->span, "Not every path returns a value", &context.name);
		}
	} else if (const ExpressionBody *bodyExpr = std::get_if<ExpressionBody>(&body.value)) {
		if (const ExpressionInfo result = expression(*bodyExpr->expression, &returns, returns.empty()); result.fallsThrough) {
			require(result.types, returns, bodyExpr->expression->get_span(), returns.empty() ? Code::VALUE_IN_VOID : Code::RETURN_TYPE);
		}
	}
}

void BodyChecker::check () {
	const DeclarationInfo &info = analyzer.declarations.at(&context);
	if (const CallableSignature *signature = callable(context)) {
		for (std::size_t i = 0; i < signature->parameters.size(); ++i) {
			const ParameterDeclaration &param = signature->parameters[i];
			if (param.defaultValue) {
				expect(*param.defaultValue, info.parameters[i]);
			}
			bind(param.name, info.parameters[i], param.isMutable());
		}
		if (const auto *ctor = dynamic_cast<const ConstructorDeclaration *>(signature)) {
			auto group = analyzer.members(selfType(), owner->name.get_lexeme(), SymbolKind::Constructor);
			std::erase_if(group, [&] (const Candidate &c) { return c.symbol == &context; });
			const ConstructorDelegation &d        = ctor->delegation;
			const ExpressionInfo         selected = invoke(d.name, d.span, d.suppliedArguments, group);
			if (record) {
				analyzer.program.expressions[&d] = selected;
			}
		}
		if (const FunctionBody *body = signature->executableBody()) {
			if (info.inferResult) {
				const auto        &inferred                    = std::get<ExpressionBody>(body->value);
				const ResolvedType result                      = scalar(*inferred.expression);
				analyzer.declarations.at(&context).returns     = { result };
				analyzer.declarations.at(&context).inferResult = false;
			} else {
				checkBody(*body);
			}
		}
		return;
	}
	if (context.kind == SymbolKind::StateInitializer) {
		checkBody(std::get<const StateInitializerDeclaration *>(context.declaration)->body);
		return;
	}
	const ClassDeclaration *cd        = nullptr;
	const EnumDeclaration  *ed        = nullptr;
	const Symbol           *container = context.kind == SymbolKind::Type ? &context : owner;
	if (container && container->kind == SymbolKind::Type) {
		const auto td = std::get<const TypeDeclaration *>(container->declaration);
		cd            = dynamic_cast<const ClassDeclaration *>(td);
		ed            = dynamic_cast<const EnumDeclaration *>(td);
	}
	if (cd && (context.kind == SymbolKind::Field || context.kind == SymbolKind::Type)) {
		owner = container;
		for (const ClassParameterDeclaration &p : cd->parameters) {
			bind(p.parameter.name, analyzer.type(p.parameter.type, *owner), p.parameter.isMutable());
		}
	}
	if (context.kind == SymbolKind::Type && cd) {
		for (std::size_t i = 0; i < cd->parents.size(); ++i) {
			if (cd->parents[i].constructorArguments) {
				const ResolvedType &parent = info.parents[i];
				invoke(cd->parents[i].type.get_name(), cd->parents[i].span, *cd->parents[i].constructorArguments,
				       analyzer.members(parent, parent.name, SymbolKind::Constructor));
			}
		}
	} else if (context.kind == SymbolKind::Field) {
		if (const auto field = std::get_if<const ClassFieldDeclaration *>(&context.declaration)) {
			expect(*(*field)->initializer, *info.value);
		} else if (const auto field = std::get_if<const EnumFieldDeclaration *>(&context.declaration); field && (*field)->defaultValue) {
			expect(*(*field)->defaultValue, *info.value);
		}
	} else if (context.kind == SymbolKind::EnumCase) {
		const EnumCaseDeclaration &ec = *std::get<const EnumCaseDeclaration *>(context.declaration);
		if (ec.values.size() > ed->fields.size()) {
			fail(Code::INVALID_ENUM_MEMBER, ec.name.get_span(), "Too many enum payload values");
		}
		for (std::size_t i = 0; i < ed->fields.size(); ++i) {
			if (i < ec.values.size()) {
				expect(*ec.values[i], analyzer.type(ed->fields[i].type, *owner));
			} else if (!ed->fields[i].defaultValue) {
				fail(Code::INVALID_ENUM_MEMBER, ec.name.get_span(), "Missing enum payload value", &ed->fields[i].name);
			}
		}
	} else if (context.kind == SymbolKind::State || context.kind == SymbolKind::StateAttachment) {
		const StateSelectionDeclaration *selection = nullptr;
		if (context.kind == SymbolKind::State) {
			selection = dynamic_cast<const NestedStateDeclaration *>(std::get<const StateBodyDeclaration *>(context.declaration));
		} else {
			selection = std::get<const StateAttachmentDeclaration *>(context.declaration);
		}
		if (selection && selection->initial) {
			if (selection->initial->payload) {
				expect(*selection->initial->payload, *info.value);
			} else if (info.value->name != to_string(kVoid)) {
				fail(Code::INVALID_STATE_PAYLOAD, selection->initial->span, "Initial state needs a payload");
			}
		}
	}
}

ExpressionInfo BodyChecker::block (const std::vector<StatementPtr> &statements, const ResultTypes *expected, const bool value) {
	scopes.emplace_back();

	struct ScopeExit {
		decltype(scopes) &stack;

		~ScopeExit () {
			stack.pop_back();
		}
	} scopeExit{ scopes };

	ExpressionInfo             result;
	std::optional<BodyChecker> unreachable;
	for (const StatementPtr &stmt : statements) {
		if (!result.fallsThrough) {
			if (!unreachable) unreachable.emplace(*this);
			unreachable->statement(*stmt);
			continue;
		}
		if (value && &stmt == &statements.back()) {
			if (const auto *last = dynamic_cast<const ExpressionStatement *>(stmt.get())) {
				const ExpressionInfo tail = expression(*last->expression, expected);
				if (result.fallsThrough) {
					result = tail;
				}
				continue;
			}
		}
		const bool next = statement(*stmt);
		result.fallsThrough &= next;
	}
	return result;
}

void BodyChecker::refine (const Expression &test, const bool truth) {
	if (const UnaryExpression *neg = dynamic_cast<const UnaryExpression *>(&test); neg && neg->op.get_operator() == oNot) {
		refine(*neg->operand, !truth);
		return;
	}
	const auto *binary = dynamic_cast<const BinaryExpression *>(&test);
	if (!binary) {
		return;
	}
	const Operator o_operation = binary->op.get_operator();
	if ((o_operation == oAnd && truth) || (o_operation == oOr && !truth)) {
		refine(*binary->left, truth);
		refine(*binary->right, truth);
		return;
	}
	if (!((o_operation == oNotEqual && truth) || (o_operation == oEqual && !truth))) {
		return;
	}
	const Expression *operand = nullptr;
	if (dynamic_cast<const NullLiteral *>(binary->left.get())) {
		operand = binary->right.get();
	}
	if (dynamic_cast<const NullLiteral *>(binary->right.get())) {
		operand = binary->left.get();
	}
	const auto *name = dynamic_cast<const NameExpression *>(operand);
	if (const Binding *binding = name ? local(name->name.get_lexeme()) : nullptr; binding && binding->type.nullable()) {
		narrowed[binding->name] = binding->type.element();
	}
}

ExpressionInfo BodyChecker::condition (const IfExpression &node, const ResultTypes *expected, const bool discard) {
	std::optional<ResultTypes> common;
	bool                       fallsThrough = false;
	std::optional<Narrowing>   joined;
	const auto                 join = [&] {
                if (!joined) {
                        joined = narrowed;
                } else {
                        std::erase_if(*joined, [&] (const Narrowing::value_type &fact) {
                                const auto it = narrowed.find(fact.first);
                                return it == narrowed.end() || it->second != fact.second;
                        });
                }
	};
	const auto branch = [&] (const std::vector<StatementPtr> &statements, const TokenSpan &span) {
		const ResultTypes   *hint   = expected ? expected : common ? &*common : nullptr;
		const ExpressionInfo result = block(statements, hint, !discard);
		if (!result.fallsThrough) {
			return;
		}
		fallsThrough = true;
		join();
		if (discard) {
			return;
		}
		if (result.types.empty()) {
			fail(Code::MISSING_BRANCH_VALUE, span, "Conditional branch needs a value");
		}
		if (common) {
			if (result.types.size() == common->size()) {
				for (std::size_t i = 0; i < result.types.size(); ++i) {
					if (expected && expected->size() == common->size() && analyzer.accepts((*expected)[i], (*common)[i]) &&
					    analyzer.accepts((*expected)[i], result.types[i])) {
						(*common)[i] = (*expected)[i];
					} else if (analyzer.accepts(result.types[i], (*common)[i])) {
						(*common)[i] = result.types[i];
					}
				}
			}
			require(result.types, *common, span, Code::BRANCH_TYPE_MISMATCH);
		} else {
			common = result.types;
		}
	};
	for (const auto &[condition, statements, span] : node.branches) {
		if (const ResolvedType test = scalar(*condition); test != ResolvedType(kBoolean)) {
			fail(Code::NON_BOOLEAN_CONDITION, condition->get_span(),
			     std::format("An {} condition must be {}", to_string(kIf), to_string(kBoolean)));
		}
		const Narrowing incoming = narrowed;
		refine(*condition, true);
		branch(statements, span);
		narrowed = incoming;
		refine(*condition, false);
	}
	if (node.otherwise) {
		branch(*node.otherwise, *node.otherwiseSpan);
	} else if (!discard) {
		fail(Code::MISSING_ELSE, node.get_span(), std::format("A conditional value requires {}", to_string(kElse)));
	} else {
		fallsThrough = true;
		join();
	}
	narrowed = joined.value_or(Narrowing{});
	return { .types = common.value_or(ResultTypes{}), .symbol = nullptr, .binding = nullptr, .parameterForArgument = {}, .fallsThrough = fallsThrough };
}

void BodyChecker::assign (const Token &target, const ResolvedType &value) {
	const ExpressionInfo binding = name(target);
	if (const Binding *b = local(target.get_lexeme()); b && (!b->mutableBinding || b->deferred)) {
		fail(Code::IMMUTABLE_BINDING, target.get_span(), "Cannot assign to immutable binding '" + target.get_lexeme() + "'", b->name);
	}
	if (!binding.binding && (!binding.symbol || binding.symbol->kind != SymbolKind::Field)) {
		fail(Code::EXPECTED_BINDING, target.get_span(), "Expected a writable binding");
	}
	if (binding.symbol && std::holds_alternative<const EnumFieldDeclaration *>(binding.symbol->declaration))
		fail(Code::INVALID_FIELD_TARGET, target.get_span(), "Enum payload fields are read-only", &binding.symbol->name);
	if (const Binding *b = local(target.get_lexeme())) {
		require({ value }, { b->type }, target.get_span());
		narrowed.erase(b->name);
	} else {
		require({ value }, binding.types, target.get_span());
	}
}

bool BodyChecker::receive (const std::vector<ReceivingTarget> &targets, const Expression &producer, const std::vector<TypeReference> *annotations,
                           const bool mutableBinding) {
	ResultTypes expected;
	if (annotations) {
		if (annotations->size() > 1 && annotations->size() != targets.size())
			fail(Code::RECEPTION_ANNOTATIONS, producer.get_span(), "Specify one type per result or one shared type for a scalar");
		for (const TypeReference &annotation : *annotations) {
			ResolvedType type = analyzer.checkedType(annotation, context);
			if (type.name == to_string(kVoid)) fail(Code::VOID_TYPE, annotation.get_span(), "A binding requires a value type");
			expected.push_back(std::move(type));
		}
	} else {
		for (const ReceivingTarget &target : targets) {
			if (std::holds_alternative<ReceivingTarget::Discard>(target.value))
				expected.emplace_back();
			else if (const Binding *binding = local(target.get_token().get_lexeme()))
				expected.push_back(binding->type);
			else
				expected.push_back(name(target.get_token()).types.front());
		}
	}
	ExpressionInfo result = expression(producer, expected.empty() ? nullptr : &expected);
	if (!result.fallsThrough) return false;
	const bool broadcast = result.types.size() == 1;
	if (!broadcast && result.types.size() != targets.size()) fail(Code::RECEPTION_ARITY, producer.get_span(), "Target count does not match result count");
	if (annotations && !expected.empty() && ((broadcast && expected.size() > 1) || (!broadcast && expected.size() == 1)))
		fail(Code::RECEPTION_ANNOTATIONS, producer.get_span(), "Use one shared annotation for a scalar or one annotation per result");
	if (broadcast && !annotations) {
		if (const auto shared = std::ranges::find_if(expected, [] (const ResolvedType &t) { return !t.name.empty(); });
		    shared != expected.end() && !analyzer.accepts(*shared, result.types.front()) &&
		    std::ranges::all_of(expected, [&] (const ResolvedType &t) { return t.name.empty() || t == *shared; }))
			result.types = { scalar(producer, &*shared) };
	}
	for (std::size_t i = 0; i < targets.size(); ++i) {
		const ResolvedType &actual   = result.types[broadcast ? 0 : i];
		const ResolvedType *required = expected.empty() ? nullptr : &expected[expected.size() == 1 ? 0 : i];
		if (required && !required->name.empty()) require({ actual }, { *required }, producer.get_span());
		if (std::holds_alternative<ReceivingTarget::Discard>(targets[i].value)) continue;
		const Token &target = targets[i].get_token();
		if (annotations) {
			if (!required && actual.name == to_string(kNull))
				fail(Code::EXPLICIT_NULLABLE_TYPE, target.get_span(),
				     std::format("A {} initializer requires an explicit nullable type", to_string(kNull)));
			bind(target, required ? *required : actual, mutableBinding);
		} else
			assign(target, actual);
	}
	return true;
}

void BodyChecker::writable (const Expression &target) {
	if (const auto *field = dynamic_cast<const FieldAccessExpression *>(&target)) {
		const ExpressionInfo selected = this->field(*field);
		if (selected.symbol->kind != SymbolKind::Field || std::holds_alternative<const EnumFieldDeclaration *>(selected.symbol->declaration)) {
			fail(Code::INVALID_FIELD_TARGET, field->name.get_span(), "Enum cases and payload fields are read-only");
		}
		if (!std::holds_alternative<const StructFieldDeclaration *>(selected.symbol->declaration)) {
			return;
		}
		writable(*field->receiver);
	} else if (const auto *index = dynamic_cast<const IndexExpression *>(&target)) {
		writable(*index->array);
	} else if (const auto *name = dynamic_cast<const NameExpression *>(&target)) {
		if (const Binding *b = local(name->name.get_lexeme()); b && (!b->mutableBinding || b->deferred)) {
			fail(Code::IMMUTABLE_BINDING, name->get_span(), "Cannot modify an immutable value", b->name);
		}
	} else {
		fail(Code::INVALID_FIELD_TARGET, target.get_span(), "Assignment needs writable storage");
	}
}

bool BodyChecker::updateOperator (const Statement &statement, const ResolvedType &receiver, const Token &op, const ExpressionPtr &rhs) {
	const std::vector<Candidate> candidates = analyzer.members(receiver, op.get_lexeme(), SymbolKind::Operator);
	if (candidates.empty()) {
		return false;
	}
	std::vector<CallArgument> arguments;
	if (rhs) {
		arguments.push_back({ .expression = rhs });
	}
	const ExpressionInfo selected = invoke(op, statement.get_span(), arguments, candidates);
	if (record) {
		analyzer.program.writes[&statement] = selected;
	}
	return true;
}

void BodyChecker::writeMember (const Statement &statement, const ExpressionPtr &target, const Token &op, const ExpressionPtr &rhs,
                               const ExpressionPtr &compound) {
	if (const auto *index = dynamic_cast<const IndexExpression *>(target.get())) {
		if (const ResolvedType receiver = scalar(*index->array); !receiver.array() && receiver.declaration) {
			const auto setters = analyzer.members(receiver, to_string(oIndexWrite), SymbolKind::Operator);
			if (setters.empty()) {
				fail(Code::INVALID_FIELD_TARGET, target->get_span(),
				     std::format("Indexed assignment needs a {} operator", to_string(oIndexWrite)));
			}
			if (compound || rhs) {
				const ExpressionInfo selected = invoke(
				        op, statement.get_span(), { { .expression = index->index }, { .expression = compound ? compound : rhs } }, setters);
				if (record) {
					analyzer.program.writes[&statement] = selected;
				}
				return;
			}
			// An indexed postfix update reads the element and stores its updated value through the setter.
			if (const ResolvedType read = scalar(*target); !read.integer() && !read.floating()) {
				fail(Code::UNARY_OPERAND, op.get_span(), "Postfix update needs a numeric element");
			}
			const ExpressionInfo selected = invoke(op, statement.get_span(), { { .expression = index->index }, { .expression = target } }, setters);
			if (record) {
				analyzer.program.writes[&statement] = selected;
			}
			return;
		}
	}
	const ResolvedType rt = scalar(*target);
	writable(*target);
	if ((compound || !rhs) && updateOperator(statement, rt, op, rhs)) {
		return;
	}
	if (compound || rhs) {
		expect(compound ? *compound : *rhs, rt);
	} else if (!rt.integer() && !rt.floating()) {
		fail(Code::UNARY_OPERAND, op.get_span(), "Update needs a numeric target");
	}
}

bool BodyChecker::statement (const Statement &node) {
	if (const auto *s = dynamic_cast<const ExpressionStatement *>(&node)) {
		return expression(*s->expression, nullptr, true).fallsThrough;
	}
	if (const auto *s = dynamic_cast<const ReturnStatement *>(&node)) {
		if (!s->value && !returns.empty()) {
			fail(Code::RETURN_VALUE_REQUIRED, s->span, "A return value is required");
		}
		if (s->value) {
			if (const ExpressionInfo result = expression(*s->value, &returns); result.fallsThrough) {
				require(result.types, returns, s->span, returns.empty() ? Code::VALUE_IN_VOID : Code::RETURN_TYPE);
			}
		}
		return false;
	}
	if (const auto *s = dynamic_cast<const BindingDeclaration *>(&node)) {
		std::optional<ResolvedType> annotation;
		if (s->type) {
			annotation = analyzer.checkedType(*s->type, context);
		}
		const ResolvedType actual = scalar(*s->initializer, annotation ? &*annotation : nullptr);
		if (annotation) {
			require({ actual }, { *annotation }, s->initializer->get_span());
		}
		if (!annotation && actual.name == to_string(kNull)) {
			fail(Code::EXPLICIT_NULLABLE_TYPE, s->span, std::format("A {} initializer requires an explicit nullable type", to_string(kNull)));
		}
		bind(s->name, annotation.value_or(actual), s->isMutable, dynamic_cast<const FunctionBlockExpression *>(s->initializer.get()) != nullptr);
	} else if (const auto *s = dynamic_cast<const BindingGroupDeclaration *>(&node)) {
		return receive(s->targets, *s->initializer, &s->types, s->isMutable);
	} else if (const auto *s = dynamic_cast<const AssignmentStatement *>(&node)) {
		const Binding     *binding = local(s->name.get_lexeme());
		const ResolvedType type    = binding ? binding->type : name(s->name).types.front();
		if (const BinaryExpression *compound = s->get_compound();
		    compound && updateOperator(node, scalar(*compound->left), *s->compoundOperator, compound->right)) {
			assign(s->name, type);
		} else {
			assign(s->name, scalar(*s->value, &type));
		}
	} else if (const auto *s = dynamic_cast<const MultiAssignmentStatement *>(&node)) {
		return receive(s->targets, *s->value, nullptr);
	} else if (const auto *s = dynamic_cast<const UpdateStatement *>(&node)) {
		const ResolvedType rt = scalar(*s->target);
		if (!updateOperator(node, rt, s->op, nullptr) && !rt.integer() && !rt.floating()) {
			fail(Code::UNARY_OPERAND, s->op.get_span(), "Update needs a numeric binding or a matching operator");
		}
		assign(s->name, rt);
	} else if (const auto *s = dynamic_cast<const ElementAssignmentStatement *>(&node)) {
		writeMember(node, s->target, s->op, s->value, s->compound);
	} else if (const auto *s = dynamic_cast<const FieldWriteStatement *>(&node)) {
		writeMember(node, s->target, s->op, s->value, s->compound);
	} else if (const auto *s = dynamic_cast<const LoopJumpStatement *>(&node)) {
		if (!loopDepth) {
			fail(Code::INVALID_LOOP_JUMP, s->get_span(), "Loop jump outside a loop");
		}
		if (s->isContinue()) {
			loopMayContinue = true;
		} else {
			loopMayBreak = true;
		}
		return false;
	} else if (const auto *s = dynamic_cast<const LoopStatement *>(&node)) {
		++loopDepth;
		// A mutable local can change between iterations; only the loop test establishes fresh facts.
		for (const auto &scope : scopes) {
			for (const auto &entries : scope | std::views::values) {
				if (entries.mutableBinding) {
					narrowed.erase(entries.name);
				}
			}
		}
		const bool outerBreak = std::exchange(loopMayBreak, false), outerContinue = std::exchange(loopMayContinue, false);
		bool       fallsThrough = true;
		if (const auto *f = dynamic_cast<const ForStatement *>(s)) {
			std::optional<ResolvedType> annotation;
			if (f->binding.type) {
				annotation = analyzer.checkedType(*f->binding.type, context);
			}
			ResolvedType rt;
			if (const ForSource::Range *range = std::get_if<ForSource::Range>(&f->source.value)) {
				rt = scalar(*range->start, annotation ? &*annotation : nullptr);
				if (!rt.integer()) {
					fail(Code::INVALID_FOR_LOOP, range->span, "Range endpoint must be an integer");
				}
				expect(*range->end, rt);
			} else {
				if (const ResolvedType source = scalar(*std::get<ForSource::Elements>(f->source.value).expression); source.array()) {
					rt = source.element();
				} else if (source.name == "range" && !source.arguments.empty()) {
					rt = source.arguments.front();
				} else {
					fail(Code::INVALID_FOR_LOOP, f->source.get_span(), "Expected an array or range");
				}
			}
			if (annotation) {
				require({ rt }, { *annotation }, f->binding.get_span());
			}
			scopes.emplace_back();
			bind(f->binding.name, annotation.value_or(rt), true);
			if (f->stepper) {
				remember(f->stepper->target, f->binding.name);
				if (f->stepper->value) {
					expect(*f->stepper->value, rt);
				}
			}
			block(f->body.statements);
			scopes.pop_back();
		} else {
			const auto          *w    = dynamic_cast<const WhileStatement *>(s);
			const auto          *d    = dynamic_cast<const DoWhileStatement *>(s);
			const ExpressionPtr &test = w ? w->condition : d->condition;
			if (w) {
				expect(*test, ResolvedType(kBoolean));
				refine(*test, true);
			}
			const ExpressionInfo body = block(w ? w->body.statements : d->body.statements);
			if (d) {
				expect(*test, ResolvedType(kBoolean));
			}
			const auto *literal = dynamic_cast<const BooleanLiteral *>(test.get());
			fallsThrough        = loopMayBreak || (!(literal && literal->value) && (w || body.fallsThrough || loopMayContinue));
		}
		--loopDepth;
		for (const auto &scope : scopes) {
			for (const auto &entry : scope | std::views::values) {
				if (entry.mutableBinding) {
					narrowed.erase(entry.name);
				}
			}
		}
		loopMayBreak    = outerBreak;
		loopMayContinue = outerContinue;
		return fallsThrough;
	} else {
		fail(Code::INTERNAL, node.get_span(), "Unrecognized statement in semantic analysis");
	}
	return true;
}

}
