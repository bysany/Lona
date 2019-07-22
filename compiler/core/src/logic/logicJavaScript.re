open LogicUtils;
open Operators;

let createVariableOrProperty =
    (
      isStaticContext: bool,
      isDynamic,
      name: string,
      value: JavaScriptAst.node,
    )
    : JavaScriptAst.node =>
  if (isStaticContext) {
    if (isDynamic) {
      MethodDefinition({
        key: "get " ++ name,
        value:
          FunctionExpression({id: None, params: [], body: [Return(value)]}),
      });
    } else {
      Property({key: Identifier([name]), value: Some(value)});
    };
  } else {
    VariableDeclaration(
      AssignmentExpression({left: Identifier([name]), right: value}),
    );
  };

let sharedPrefix =
    (~rootNode: LogicAst.syntaxNode, ~a: string, ~b: string): list(string) => {
  let rec inner = (aPath, bPath) =>
    switch (aPath, bPath) {
    | ([a, ...aRest], [b, ...bRest]) when a == b => [
        a,
        ...inner(aRest, bRest),
      ]
    | _ => []
    };
  let aPath = LogicProtocol.declarationPathTo(rootNode, a);
  let bPath = LogicProtocol.declarationPathTo(rootNode, b);
  inner(aPath, bPath);
};

let rec convert =
        (config: Config.t, node: LogicAst.syntaxNode): JavaScriptAst.node => {
  let context = {config, isStatic: false, rootNode: node};
  /* Js.log(LogicProtocol.nodeHierarchyDescription(node, ())); */
  switch (node) {
  | LogicAst.Program(Program(contents)) => program(context, contents)
  | LogicAst.TopLevelDeclarations(TopLevelDeclarations(contents)) =>
    topLevelDeclarations(context, contents)
  | _ =>
    Js.log("Unhandled syntaxNode type");
    Empty;
  };
}
and program =
    (context: context, node: LogicAst.programProgram): JavaScriptAst.node =>
  JavaScriptAst.Program(
    node.block
    |> unfoldPairs
    |> Sequence.rejectWhere(isPlaceholderStatement)
    |> List.map(statement(context)),
  )
and topLevelDeclarations =
    (
      context: context,
      node: LogicAst.topLevelDeclarationsTopLevelDeclarations,
    )
    : JavaScriptAst.node =>
  JavaScriptAst.Program(
    node.declarations
    |> unfoldPairs
    |> Sequence.rejectWhere(isPlaceholderDeclaration)
    |> List.map(declaration(context)),
  )
and statement =
    (context: context, node: LogicAst.statement): JavaScriptAst.node =>
  switch (node) {
  | Declaration({content}) => declaration(context, content)
  | Placeholder(_) => Empty
  | _ =>
    Js.log("Unhandled statement type");
    Empty;
  }
and declaration =
    (context: context, node: LogicAst.declaration): JavaScriptAst.node =>
  switch (node) {
  | ImportDeclaration(_) => Empty
  | Namespace({name: LogicAst.Pattern({name}), declarations}) =>
    let newContext = {...context, isStatic: true};

    let variable =
      createVariableOrProperty(
        context.isStatic,
        false,
        String.lowercase(name),
        ObjectLiteral(
          declarations
          |> unfoldPairs
          |> Sequence.rejectWhere(isPlaceholderDeclaration)
          |> List.map(declaration(newContext)),
        ),
      );

    if (context.isStatic) {
      variable;
    } else {
      ExportNamedDeclaration(variable);
    };
  | Variable({id, name: LogicAst.Pattern({name}), initializer_}) =>
    let initialValue =
      (initializer_ |> Monad.map(expression(context)))
      %? Identifier(["undefined"]);

    let isDynamic =
      LogicAst.Declaration(node)
      |> LogicTraversal.reduce(
           LogicTraversal.emptyConfig(), false, (result, child, _) =>
           switch (child) {
           | Expression(IdentifierExpression({id: identifierId})) =>
             let prefix =
               sharedPrefix(
                 ~rootNode=context.rootNode,
                 ~a=id,
                 ~b=identifierId,
               );
             if (prefix == []) {
               result;
             } else {
               true;
             };
           | _ => result
           }
         );

    createVariableOrProperty(
      context.isStatic,
      isDynamic,
      String.lowercase(name),
      initialValue,
    );
  | Record({
      name: LogicAst.Pattern({name}),
      genericParameters: _,
      /* declarations, */
    }) =>
    Empty
  | Enumeration({name: LogicAst.Pattern({name}), cases}) =>
    VariableDeclaration(
      AssignmentExpression({
        left: Identifier([JavaScriptFormat.enumName(name)]),
        right:
          ObjectLiteral(
            cases
            |> unfoldPairs
            |> List.map((enumCase: LogicAst.enumerationCase) =>
                 switch (enumCase) {
                 | Placeholder(_) => None
                 | EnumerationCase(value) => Some(value)
                 }
               )
            |> Sequence.compact
            |> List.map((enumCase: LogicAst.enumerationCaseEnumerationCase) => {
                 /* TODO: Handle enums with associated data */
                 let {LogicAst.name: Pattern({name}), associatedValueTypes} = enumCase;

                 JavaScriptAst.Property({
                   key: Identifier([JavaScriptFormat.enumCaseName(name)]),
                   value: Some(StringLiteral(name)),
                 });
               }),
          ),
      }),
    )

  | Placeholder(_) => Empty
  | _ =>
    Js.log("Unhandled declaration type");
    Empty;
  }
and expression =
    (context: context, node: LogicAst.expression): JavaScriptAst.node =>
  switch (node) {
  | IdentifierExpression({
      identifier: Identifier({id, string: name, isPlaceholder: _}),
    }) =>
    let standard: JavaScriptAst.node = Identifier([name]);
    let scope = LogicScope.build(context.rootNode, ());
    let patternId = (scope.identifierToPattern)#get(id);
    switch (patternId) {
    | Some(patternId) =>
      let pattern = LogicProtocol.find(context.rootNode, patternId);
      switch (pattern) {
      | Some(Pattern(Pattern({id: patternId}))) =>
        let identifierPath =
          LogicProtocol.declarationPathTo(context.rootNode, patternId)
          |> List.map(Format.lowerFirst);
        Identifier(identifierPath);
      | _ => standard
      };
    | None => standard
    };

  | LiteralExpression({literal: value}) => literal(context, value)
  | MemberExpression({
      memberName: Identifier({string}),
      /* expression: innerExpression, */
    }) =>
    Identifier([string])
  /* SwiftAst.memberExpression([
       expression(context, innerExpression),
       SwiftAst.SwiftIdentifier(string),
     ]) */
  | FunctionCallExpression({arguments, expression: innerExpression}) =>
    CallExpression({
      callee: expression(context, innerExpression),
      arguments:
        arguments
        |> unfoldPairs
        |> List.map((arg: LogicAst.functionCallArgument) => {
             let LogicAst.FunctionCallArgument({
                   /* label, */
                   expression: innerExpression,
                 }) = arg;
             expression(context, innerExpression);
           }),
    })
  | Placeholder(_) =>
    Js.log("Placeholder expression remaining");
    Empty;
  | _ =>
    Js.log("Unhandled expression type");
    Empty;
  }
and literal = (context: context, node: LogicAst.literal): JavaScriptAst.node =>
  switch (node) {
  | None(_) => Identifier(["null"])
  | Boolean({value}) => Literal(LonaValue.boolean(value))
  | Number({value}) => Literal(LonaValue.number(value))
  | String({value}) => StringLiteral(value)
  | Color({value}) => StringLiteral(value)
  | Array({value}) =>
    ArrayLiteral(value |> unfoldPairs |> List.map(expression(context)))
  };