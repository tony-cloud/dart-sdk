// Copyright (c) 2024, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import 'package:front_end/src/api_prototype/dynamic_module_validator.dart'
    show DynamicInterfaceLanguageImplPragmas, DynamicInterfaceSpecification;
import 'package:kernel/ast.dart';
import 'package:kernel/class_hierarchy.dart' show ClassHierarchy;
import 'package:kernel/core_types.dart' show CoreTypes;

import 'package:vm/modular/transformations/pragma.dart'
    show
        dynModuleCanBeOverriddenPragmaName,
        dynModuleCallablePragmaName,
        dynModuleExtendablePragmaName,
        dynModuleCanBeUsedAsTypePragmaName,
        dynModuleImplicitlyCallablePragmaName,
        dynModuleImplicitlyExtendablePragmaName,
        dynModuleDynamicallyCallablePragmaName,
        dynModuleImplicitlyDynamicallyCallablePragmaName,
        dynModuleCanBeOverriddenImplicitlyPragmaName;

/// The opt-in Shorebird baseline keeps the host API and hierarchy open while
/// using the bytecode interpreter, without enabling Dart Dynamic Modules.
/// Include private declarations of application libraries: a patch can add code
/// in an existing library. These annotations constrain compiler assumptions;
/// Dart language restrictions on extending classes and overriding members
/// still apply to patch sources.
///
/// `dart:` libraries follow a smaller retention rule, because patches cannot
/// add code to them and can never name their private declarations. Every
/// annotation below is an AOT root (callable and dynamically-callable pragmas
/// are entry points), so annotating private `dart:` declarations only bloats
/// the snapshot (dynamic invocation forwarders, tear-offs, method extractors,
/// Function objects and code source maps; `dart:typed_data` dominates).
///
/// * Public `dart:` classes: annotated as before (callable, and extendable and
///   overridable unless final, sealed; see [_isClosedToPatches]). Their private
///   members are not annotated: another library cannot override or call them.
/// * Private `dart:` classes: no class annotation. They are retained only if the
///   base program allocates them (normal reachability), for example through
///   `List()` returning a `_GrowableList` or `Float32List(n)` returning a
///   `_Float32List`.
/// * Public instance members of private `dart:` classes are annotated
///   dynamically callable only if they implement a public selector, i.e.
///   the same name (and getter/setter kind) is declared by a public supertype
///   (including `Object`). A patch can call exactly those on an object of a
///   private runtime class through the public interface (`list.add(1)`, a
///   tear-off `list.add`, or `(x as dynamic).add(1)`). With
///   `--shorebird-structural-patching` the AOT precompiler does not treat a
///   dynamically-callable-only pragma as a root: the member (and its dynamic
///   invocation forwarder) is kept exactly when the class is allocated by
///   reachable code. Public-named members
///   not declared by any public supertype stay unannotated: they are only
///   reachable by a dynamic call a patch cannot justify statically.
/// * Targets of redirecting factories in `dart:` (and their classes) are
///   annotated callable: a call of the factory in a patch is compiled as a call
///   of the (private) target constructor.
/// * Private `dart:` classes with a const constructor are annotated callable
///   too, and so are the instance fields of every `dart:` class with a const
///   constructor: constants created by a patch (`Endian.little`,
///   `Abi.macosArm64` with its private `_OS`, enum values) refer to the class
///   and to every field by name.
/// * Private top-level members and private statics of `dart:` libraries are
///   not annotated, except those the bytecode compiler itself references
///   ([_bytecodeSdkDependencies], string interpolation, `async`, late
///   variables, ...). `pkg/vm/test/transformations/` checks that list against
///   `dart2bytecode`'s `LibraryIndex` lookups.
void annotateShorebirdStructuralComponent(
  Component component,
  CoreTypes coreTypes,
) {
  final extendable = pragmaConstant(coreTypes, dynModuleExtendablePragmaName);
  final callable = pragmaConstant(coreTypes, dynModuleCallablePragmaName);
  final overridable = pragmaConstant(
    coreTypes,
    dynModuleCanBeOverriddenPragmaName,
  );
  final dynamicCallable = pragmaConstant(
    coreTypes,
    dynModuleDynamicallyCallablePragmaName,
  );
  final publicSelectors = _PublicSelectors();
  // The CFE resolves calls to a redirecting factory (`factory MapEntry(k, v) =
  // MapEntry._`) to its target, so a patch refers to the (possibly private)
  // target constructor and its class directly.
  final redirectionTargets = Set<Member>.identity();
  final redirectionTargetClasses = Set<Class>.identity();
  for (final library in component.libraries) {
    if (!library.importUri.isScheme('dart')) continue;
    for (final cls in library.classes) {
      for (final procedure in cls.procedures) {
        if (!procedure.isRedirectingFactory) continue;
        final target = procedure.function.redirectingFactoryTarget?.target;
        if (target == null) continue;
        redirectionTargets.add(target);
        final targetClass = target.enclosingClass;
        if (targetClass != null) redirectionTargetClasses.add(targetClass);
      }
    }
  }
  for (final library in component.libraries) {
    final isCoreLibrary = library.importUri.isScheme('dart');
    final required = _SdkDependencies(
      _bytecodeSdkDependencies[library.importUri.toString()] ??
          const <String>{},
    );
    for (final cls in library.classes) {
      final isPrivateCoreClass = isCoreLibrary && cls.name.startsWith('_');
      final isOpenPrivateClass =
          isPrivateCoreClass && required.classIsOpen(cls.name);
      final isConstClass =
          isCoreLibrary && cls.constructors.any((c) => c.isConst);
      final canBeSubtyped = !(isCoreLibrary && _isClosedToPatches(cls));
      if (canBeSubtyped) {
        cls.addAnnotation(ConstantExpression(extendable));
      }
      if (!isPrivateCoreClass ||
          isOpenPrivateClass ||
          isConstClass ||
          redirectionTargetClasses.contains(cls)) {
        cls.addAnnotation(ConstantExpression(callable));
      }
      for (final member in cls.members) {
        final isRequired =
            required.hasMember(cls.name, member.name.text) ||
            (isOpenPrivateClass && !member.isInstanceMember) ||
            redirectionTargets.contains(member) ||
            // A patch can create constants of a class with a const
            // constructor (`Endian.little`, `Abi.macosArm64`, enum values);
            // the bytecode refers to every instance field by name.
            (isConstClass && member is Field && member.isInstanceMember);
        if (isCoreLibrary && !isRequired) {
          if (member.name.isPrivate || member.name.text.startsWith('_')) {
            continue;
          }
          if (isPrivateCoreClass) {
            if (member.isInstanceMember &&
                publicSelectors.implementsPublicSelector(cls, member)) {
              // Only dynamically callable (not an unconditional AOT root): the
              // member is retained when the base program allocates [cls].
              member.addAnnotation(ConstantExpression(dynamicCallable));
            }
            continue;
          }
        }
        member.addAnnotation(ConstantExpression(callable));
        if (member.isInstanceMember) {
          if (canBeSubtyped) {
            member.addAnnotation(ConstantExpression(overridable));
          }
          member.addAnnotation(ConstantExpression(dynamicCallable));
        }
      }
    }
    for (final member in library.members) {
      if (isCoreLibrary &&
          (member.name.isPrivate || member.name.text.startsWith('_')) &&
          !required.hasMember('', member.name.text)) {
        continue;
      }
      member.addAnnotation(ConstantExpression(callable));
    }
  }
}

/// Whether code outside the declaring `dart:` library can never subtype [cls].
bool _isClosedToPatches(Class cls) =>
    cls.name.startsWith('_') || cls.isFinal || cls.isSealed;

/// Private `dart:` declarations that the compilers of a patch (the CFE and VM
/// kernel lowerings and `dart2bytecode`) reference by name, as `Class` (the
/// class and all its constructors and statics), `Class.member` or `.topLevel`
/// (any getter/setter/method of that name; a trailing `*` matches a prefix).
/// A patch that uses the corresponding language feature (string interpolation,
/// `async`, `late` fields and locals, list, map and set literals, enums, `is`
/// tests, FFI, ...) needs these even if the base program does not.
///
/// Generated from the `LibraryIndex` lookups in `pkg/kernel/lib/core_types.dart`,
/// `pkg/vm/lib/**` and `pkg/dart2bytecode/lib/**`; the test
/// `shorebird_structural_annotator_test.dart` scans those sources and fails if
/// a lookup of a private `dart:` declaration is missing here.
const Map<String, Set<String>> _bytecodeSdkDependencies = {
  'dart:_compact_hash': {'_ConstMap', '_ConstSet', '_Map', '_Set'},
  'dart:_internal': {
    '._ensureDeeplyImmutable',
    '._instantiateClosure',
    '._nativeEffect',
    '._prependTypeArguments',
    'LateError._throwLocalAlreadyInitialized',
    'LateError._throwLocalAssignedDuringInitialization',
    'LateError._throwLocalNotInitialized',
  },
  'dart:_late_helper': {
    '._lateInitializeOnceCheck',
    '._lateReadCheck',
    '._lateWriteOnceCheck',
    '_Cell',
    '_Cell.finalFieldValue',
    '_Cell.finalLocalValue',
    '_Cell.named',
    '_Cell.readField',
    '_Cell.readLocal',
    '_Cell.value',
    '_InitializedCell',
    '_InitializedCell.finalValue',
    '_InitializedCell.named',
    '_InitializedCell.read',
    '_InitializedCell.readFinal',
    '_InitializedCell.value',
  },
  'dart:async': {
    '._asyncStarMoveNextHelper',
    '._clearAsyncThreadStackTrace',
    '._setAsyncThreadStackTrace',
    '._wrapAwaitedExpression',
    '_AsyncStarStreamController.add',
    '_AsyncStarStreamController.addStream',
    '_Future',
    '_StreamIterator',
    '_StreamIterator._subscription',
    '_StreamIterator.cancel',
    '_StreamIterator.current',
    '_StreamIterator.moveNext',
    '_SuspendState._await',
    '_SuspendState._awaitWithTypeCheck',
    '_SuspendState._functionData',
    '_SuspendState._handleException',
    '_SuspendState._initAsync',
    '_SuspendState._initAsyncStar',
    '_SuspendState._initSyncStar',
    '_SuspendState._returnAsync',
    '_SuspendState._returnAsyncStar',
    '_SuspendState._suspendSyncStarAtStart',
    '_SuspendState._yieldAsyncStar',
    '_SyncStarIterable',
    '_SyncStarIterator._current',
    '_SyncStarIterator._yieldStarIterable',
  },
  'dart:core': {
    // Not a LibraryIndex lookup: the CFE makes every enum extend `_Enum`.
    '_Enum',
    '._checkLoaded',
    '._loadLibrary',
    'Map._fromLiteral',
    'NoSuchMethodError._throwNew',
    'Object._instanceOf',
    'Object._simpleInstanceOf',
    'StateError._throwNew',
    '_AssertionError._throwNewSource',
    '_Closure',
    '_Double',
    '_Enum._name',
    '_GrowableList',
    '_GrowableList._literal',
    '_GrowableList._literal*',
    '_GrowableList.empty',
    '_GrowableList.filled',
    '_GrowableList.generate',
    '_ImmutableList',
    '_InvocationMirror',
    '_InvocationMirror._allocateInvocationMirror',
    '_InvocationMirror._withType',
    '_List',
    '_List.empty',
    '_List.filled',
    '_List.generate',
    '_OneByteString',
    '_Record',
    '_Smi',
    '_StringBase._interpolate',
    '_StringBase._interpolateSingle',
    '_TwoByteString',
    'pragma._',
  },
  'dart:ffi': {
    '._abi',
    '._checkAbiSpecificIntegerMapping',
    '._createNativeCallableIsolateGroupBound',
    '._createNativeCallableIsolateLocal',
    '._createNativeCallableListener',
    '._ffiCall',
    '._fromAddress',
    '._load*',
    '._loadAbiSpecificInt',
    '._loadAbiSpecificIntAtIndex',
    '._memCopy',
    '._nativeAsyncCallbackFunction',
    '._nativeCallbackFunction',
    '._nativeIsolateGroupBoundCallbackFunction',
    '._nativeIsolateGroupBoundClosureFunction',
    '._nativeIsolateLocalCallbackFunction',
    '._store*',
    '._storeAbiSpecificInt',
    '._storeAbiSpecificIntAtIndex',
    'Array._',
    'Array._checkIndex',
    'Array._nestedDimensions',
    'Array._nestedDimensionsFirst',
    'Array._nestedDimensionsFlattened',
    'Array._nestedDimensionsRest',
    'Array._size',
    'Array._variableLength',
    'Native._addressOf',
    'Native._ffi_resolver',
    'Pointer._offsetBy',
    'Struct._fromTypedData',
    'Struct._fromTypedDataBase',
    'Union._fromTypedData',
    'Union._fromTypedDataBase',
    '_AbiSpecificIntegerArrayList',
    '_ArrayArrayList',
    '_ArrayList',
    '_ArraySize',
    '_ArraySize.dimension1',
    '_ArraySize.dimension2',
    '_ArraySize.dimension3',
    '_ArraySize.dimension4',
    '_ArraySize.dimension5',
    '_ArraySize.dimensions',
    '_ArraySize.variableDimension',
    '_Compound',
    '_Compound._fromTypedDataBase',
    '_Compound._offsetInBytes',
    '_Compound._typedDataBase',
    '_FfiAbiSpecificMapping',
    '_FfiAbiSpecificMapping.nativeTypes',
    '_FfiCall',
    '_FfiCall.isLeaf',
    '_FfiInlineArray',
    '_FfiInlineArray.elementType',
    '_FfiInlineArray.length',
    '_FfiInlineArray.variableLength',
    '_FfiStructLayout',
    '_FfiStructLayout.fieldTypes',
    '_FfiStructLayout.packing',
    '_NativeCallableBase._pointer',
    '_NativeCallableIsolateGroupBound',
    '_NativeCallableIsolateLocal',
    '_NativeCallableListener',
    '_NativeCallableListener._port',
  },
  'dart:nativewrappers': {'._getNativeField'},
};

/// See [_bytecodeSdkDependencies] (exposed for tests).
Map<String, Set<String>> get shorebirdBytecodeSdkDependencies =>
    _bytecodeSdkDependencies;

class _SdkDependencies {
  final Set<String> _entries;
  final List<String> _prefixes;

  _SdkDependencies(this._entries)
    : _prefixes = [
        for (final e in _entries)
          if (e.endsWith('*')) e.substring(0, e.length - 1),
      ];

  bool classIsOpen(String cls) => _entries.contains(cls);

  bool hasMember(String cls, String name) {
    final key = '$cls.$name';
    return _entries.contains(key) || _prefixes.any(key.startsWith);
  }
}

/// Finds the instance members of `dart:` classes that implement a selector
/// declared by a public supertype.
class _PublicSelectors {
  final Map<Class, Set<String>> _interface = Map.identity();
  final Map<Class, Set<String>> _inheritedPublic = Map.identity();

  static String _key(Name name, {required bool setter}) =>
      '${setter ? 'set:' : ''}${name.text}';

  static Iterable<String> _selectorsOf(Member member) sync* {
    if (member is Field) {
      yield _key(member.name, setter: false);
      if (member.hasSetter) yield _key(member.name, setter: true);
    } else if (member is Procedure && !member.isFactory) {
      yield _key(member.name, setter: member.isSetter);
    }
  }

  /// All public instance selectors of the interface of [cls] (declared in it
  /// or in any supertype).
  Set<String> _interfaceOf(Class cls) => _interface[cls] ??= {
    for (final member in cls.members)
      if (member.isInstanceMember &&
          !member.name.isPrivate &&
          !member.name.text.startsWith('_'))
        ..._selectorsOf(member),
    for (final supertype in cls.supers) ..._interfaceOf(supertype.classNode),
  };

  /// The selectors [cls] inherits through its public supertypes.
  Set<String> _publicSelectorsOf(Class cls) => _inheritedPublic[cls] ??= {
    for (final supertype in cls.supers)
      if (supertype.classNode.name.startsWith('_'))
        ..._publicSelectorsOf(supertype.classNode)
      else
        ..._interfaceOf(supertype.classNode),
  };

  bool implementsPublicSelector(Class cls, Member member) =>
      _selectorsOf(member).any(_publicSelectorsOf(cls).contains);
}

void annotateComponent(
  String dynamicInterfaceSpecification,
  Uri baseUri,
  Component component,
  CoreTypes coreTypes, {
  Map<String, List<Map<String, String>>>? detailedDynamicInterfaceJson,
}) {
  final spec = DynamicInterfaceSpecification(
    dynamicInterfaceSpecification,
    baseUri,
    component,
  );

  discoverLanguageImplPragmasInCoreLibraries(spec, component, coreTypes);

  final _DetailedDynamicInterfaceLogger? logger =
      detailedDynamicInterfaceJson != null
      ? _DetailedDynamicInterfaceLogger(detailedDynamicInterfaceJson)
      : null;

  logger?.setActiveSection('extendable');
  final extendableAnnotator = annotateNodes(
    spec.extendable,
    dynModuleExtendablePragmaName,
    baseUri,
    coreTypes,
    annotateClasses: true,
    annotateFinalClasses: false,
    annotateExtensionTypes: false,
    annotateStaticMembers: false,
    annotateInstanceMembers: false,
    logger: logger,
  );

  _ImplicitExtendableAnnotator(
    pragmaConstant(coreTypes, dynModuleImplicitlyExtendablePragmaName),
    extendableAnnotator.annotatedClasses,
  ).annotate();

  logger?.setActiveSection('can-be-overridden');
  final canBeOverriddenAnnotator = annotateNodes(
    spec.canBeOverridden,
    dynModuleCanBeOverriddenPragmaName,
    baseUri,
    coreTypes,
    annotateClasses: false,
    annotateFinalClasses: true,
    annotateExtensionTypes: false,
    annotateStaticMembers: false,
    annotateInstanceMembers: true,
    logger: logger,
  );

  final hierarchy = ClassHierarchy(component, coreTypes);

  copyCanBeOverriddenToMixinApplications(
    canBeOverriddenAnnotator.annotatedMembers,
    canBeOverriddenAnnotator.pragma,
    component,
    hierarchy,
  );

  annotateMembersOfExtendableMixinsAsCanBeOverridden(
    extendableAnnotator.annotatedClasses,
    canBeOverriddenAnnotator.annotatedMembers,
    canBeOverriddenAnnotator.pragma,
  );

  _ImplicitOverridesAnnotator(
    pragmaConstant(coreTypes, dynModuleCanBeOverriddenImplicitlyPragmaName),
    hierarchy,
    canBeOverriddenAnnotator.annotatedMembers,
  ).annotate();

  logger?.setActiveSection('callable');
  final callableAnnotator = annotateNodes(
    spec.callable,
    dynModuleCallablePragmaName,
    baseUri,
    coreTypes,
    annotateClasses: true,
    annotateFinalClasses: true,
    annotateExtensionTypes: true,
    annotateStaticMembers: true,
    annotateInstanceMembers: true,
    logger: logger,
  );

  final implicitUsesAnnotator = _ImplicitUsesAnnotator(
    pragmaConstant(coreTypes, dynModuleImplicitlyCallablePragmaName),
    callableAnnotator.annotatedClasses,
    callableAnnotator.annotatedMembers,
  );

  implicitUsesAnnotator.annotateMixinUses(extendableAnnotator.annotatedClasses);
  implicitUsesAnnotator.annotateMemberUses(callableAnnotator.annotatedMembers);
  implicitUsesAnnotator.annotateDispatchTargets(component);

  logger?.setActiveSection('can-be-used-as-type');
  annotateNodes(
    spec.canBeUsedAsType,
    dynModuleCanBeUsedAsTypePragmaName,
    baseUri,
    coreTypes,
    annotateClasses: true,
    annotateFinalClasses: true,
    annotateExtensionTypes: true,
    annotateStaticMembers: false,
    annotateInstanceMembers: false,
    logger: logger,
  );
  logger?.setActiveSection('dynamically-callable');
  final dynamicallyCallableAnnotator = annotateNodes(
    spec.dynamicallyCallable,
    dynModuleDynamicallyCallablePragmaName,
    baseUri,
    coreTypes,
    annotateClasses: false,
    annotateFinalClasses: false,
    annotateExtensionTypes: false,
    annotateStaticMembers: false,
    annotateInstanceMembers: true,
    logger: logger,
  );

  final dynamicallyCallableImplicitUsesAnnotator = _ImplicitUsesAnnotator(
    pragmaConstant(coreTypes, dynModuleImplicitlyDynamicallyCallablePragmaName),
    dynamicallyCallableAnnotator.annotatedClasses,
    dynamicallyCallableAnnotator.annotatedMembers,
  );
  dynamicallyCallableImplicitUsesAnnotator.annotateDispatchTargets(component);
}

InstanceConstant pragmaConstant(CoreTypes coreTypes, String pragmaName) {
  return InstanceConstant(coreTypes.pragmaClass.reference, [], {
    coreTypes.pragmaName.fieldReference: StringConstant(pragmaName),
    coreTypes.pragmaOptions.fieldReference: NullConstant(),
  });
}

_Annotator annotateNodes(
  Set<TreeNode> nodes,
  String pragmaName,
  Uri baseUri,
  CoreTypes coreTypes, {
  required bool annotateClasses,
  required bool annotateFinalClasses,
  required bool annotateExtensionTypes,
  required bool annotateStaticMembers,
  required bool annotateInstanceMembers,
  _DetailedDynamicInterfaceLogger? logger,
}) {
  final pragma = pragmaConstant(coreTypes, pragmaName);
  final annotator = _Annotator(
    pragma,
    annotateClasses: annotateClasses,
    annotateFinalClasses: annotateFinalClasses,
    annotateExtensionTypes: annotateExtensionTypes,
    annotateStaticMembers: annotateStaticMembers,
    annotateInstanceMembers: annotateInstanceMembers,
    logger: logger,
  );
  for (final node in nodes) {
    node.accept(annotator);
  }
  return annotator;
}

class _Annotator extends RecursiveVisitor {
  final Constant pragma;

  final bool annotateClasses;
  final bool annotateFinalClasses;
  final bool annotateExtensionTypes;
  final bool annotateStaticMembers;
  final bool annotateInstanceMembers;
  final _DetailedDynamicInterfaceLogger? logger;

  final Set<Class> annotatedClasses = Set<Class>.identity();
  final Set<Member> annotatedMembers = Set<Member>.identity();

  _Annotator(
    this.pragma, {
    required this.annotateClasses,
    required this.annotateFinalClasses,
    required this.annotateExtensionTypes,
    required this.annotateStaticMembers,
    required this.annotateInstanceMembers,
    this.logger,
  });

  bool get annotateMembers => annotateStaticMembers || annotateInstanceMembers;

  @override
  void visitLibrary(Library node) {
    for (final c in node.classes) {
      if (c.name[0] != '_') {
        c.accept(this);
      }
    }
    for (final ext in node.extensions) {
      if (ext.name[0] != '_') {
        ext.accept(this);
      }
    }
    for (final extensionType in node.extensionTypeDeclarations) {
      if (extensionType.name[0] != '_') {
        extensionType.accept(this);
      }
    }
    for (final exportRef in node.additionalExports) {
      exportRef.node!.accept(this);
    }
    if (annotateMembers) {
      _visitPublicMembers(node.procedures);
      _visitPublicMembers(node.fields);
    }
  }

  @override
  void visitClass(Class node) {
    annotateClass(node);
    if (annotateMembers) {
      _visitPublicMembers(node.constructors);
      _visitPublicMembers(node.procedures);
      _visitPublicMembers(node.fields);
    }
  }

  void _visitPublicMembers(List<Member> members) {
    for (final m in members) {
      if (!m.name.isPrivate) {
        m.accept(this);
      }
    }
  }

  @override
  void defaultMember(Member node) {
    annotateMember(node);
  }

  @override
  void visitClassReference(Class node) {
    annotateClass(node);
  }

  void annotateClass(Class node) {
    if (annotateClasses &&
        (annotateFinalClasses || !node.isFinal) &&
        annotatedClasses.add(node)) {
      logger?.logNode(node);
      node.addAnnotation(ConstantExpression(pragma));
    }
  }

  void annotateExtensionType(ExtensionTypeDeclaration node) {
    if (annotateExtensionTypes) {
      logger?.logNode(node);
      node.addAnnotation(ConstantExpression(pragma));
    }
  }

  void annotateMember(Member node) {
    if ((node.isInstanceMember
            ? annotateInstanceMembers
            : annotateStaticMembers) &&
        annotatedMembers.add(node)) {
      logger?.logNode(node);
      node.addAnnotation(ConstantExpression(pragma));
    }
  }

  @override
  void visitExtension(Extension node) {
    for (final md in node.memberDescriptors) {
      final member = md.memberReference?.node;
      if (member != null) {
        annotateMember(member as Member);
      }
      final tearOff = md.tearOffReference?.node;
      if (tearOff != null) {
        annotateMember(tearOff as Member);
      }
    }
  }

  @override
  void visitExtensionTypeDeclaration(ExtensionTypeDeclaration node) {
    annotateExtensionType(node);
    for (final md in node.memberDescriptors) {
      final member = md.memberReference?.node;
      if (member != null) {
        annotateMember(member as Member);
      }
      final tearOff = md.tearOffReference?.node;
      if (tearOff != null) {
        annotateMember(tearOff as Member);
      }
    }
    node.declaredRepresentationType.accept(this);
  }
}

class _ImplicitExtendableAnnotator {
  final Constant pragma;
  final Set<Class> extendableClasses;
  final Set<Class> implicitlyExtendable = Set<Class>.identity();

  _ImplicitExtendableAnnotator(this.pragma, this.extendableClasses);

  void annotate() {
    for (final cls in extendableClasses) {
      annotateSupertypesOf(cls);
    }
  }

  void annotateSupertypesOf(Class cls) {
    for (final supertype in cls.supers) {
      final supertypeClass = supertype.classNode;
      if (implicitlyExtendable.add(supertypeClass) &&
          !extendableClasses.contains(supertypeClass)) {
        supertypeClass.addAnnotation(ConstantExpression(pragma));
        annotateSupertypesOf(supertypeClass);
      }
    }
  }
}

class _ImplicitOverridesAnnotator {
  final Constant pragma;
  final ClassHierarchy hierarchy;
  final Set<Member> overriddenMembers;
  final Set<Member> implicitlyOverriddenSetters = Set<Member>.identity();
  final Set<Member> implicitlyOverriddenNonSetters = Set<Member>.identity();

  _ImplicitOverridesAnnotator(
    this.pragma,
    this.hierarchy,
    this.overriddenMembers,
  );

  void annotate() {
    for (final member in overriddenMembers) {
      final cls = member.enclosingClass!;
      if (member.hasGetter) {
        annotateSupertypesOf(cls, member.name, setter: false);
      }
      if (member.hasSetter) {
        annotateSupertypesOf(cls, member.name, setter: true);
      }
    }
  }

  void annotateSupertypesOf(
    Class cls,
    Name memberName, {
    required bool setter,
  }) {
    for (final supertype in cls.supers) {
      final supertypeClass = supertype.classNode;
      final member = ClassHierarchy.findMemberByName(
        hierarchy.getDeclaredMembers(supertypeClass, setters: setter),
        memberName,
      );
      if (member != null) {
        final implicitlyOverridden = setter
            ? implicitlyOverriddenSetters
            : implicitlyOverriddenNonSetters;
        if (implicitlyOverridden.add(member) &&
            !overriddenMembers.contains(member)) {
          member.addAnnotation(ConstantExpression(pragma));
        } else {
          // The member is already annotated - do not go deeper.
          continue;
        }
      }
      annotateSupertypesOf(supertypeClass, memberName, setter: setter);
    }
  }
}

class _ImplicitUsesAnnotator extends RecursiveVisitor {
  final Constant pragma;

  final Set<Class> annotatedClasses = Set<Class>.identity();
  final Set<Member> annotatedMembers = Set<Member>.identity();
  final Set<Class> _annotatedConstantClasses = Set<Class>.identity();
  final Set<Class> _annotatedSuperTypes = Set<Class>.identity();
  final Set<Constant> _visitedConstants = Set<Constant>.identity();
  final Map<Class, _ClassInfo> _classInfos = Map<Class, _ClassInfo>.identity();

  _ImplicitUsesAnnotator(
    this.pragma,
    Set<Class> explicitlyUsedClasses,
    Set<Member> explicitlyUsedMembers,
  ) {
    annotatedClasses.addAll(explicitlyUsedClasses);
    annotatedMembers.addAll(explicitlyUsedMembers);
    explicitlyUsedClasses.forEach(annotateSuperTypes);
  }

  void annotateMixinUses(Set<Class> extendableClasses) {
    for (final cls in extendableClasses) {
      if (cls.isMixinClass || cls.isMixinDeclaration) {
        cls.visitChildren(this);
      }
    }
  }

  void annotateMemberUses(Set<Member> explicitlyUsedMembers) {
    for (final node in explicitlyUsedMembers) {
      node.acceptReference(this);
    }
  }

  @override
  void visitClassReference(Class node) {
    annotateClass(node);
  }

  @override
  void visitConstructorReference(Constructor node) {
    annotateMember(node);
    if (node.isConst) {
      annotateConstantClass(node.enclosingClass);
      node.visitChildren(this);
    }
    annotateSuperTypes(node.enclosingClass);
  }

  @override
  void visitProcedureReference(Procedure node) {
    annotateMember(node);
    if (node.isRedirectingFactory) {
      final target = node.function.redirectingFactoryTarget?.target;
      target?.acceptReference(this);
    }
    if (node.isFactory) {
      annotateSuperTypes(node.enclosingClass!);
    }
    if (node.isConst) {
      node.visitChildren(this);
    }
  }

  @override
  void visitFieldReference(Field node) {
    annotateMember(node);
    if (node.isConst) {
      (node.initializer as ConstantExpression).constant.acceptReference(this);
    }
  }

  @override
  void defaultConstantReference(Constant node) {
    if (_visitedConstants.add(node)) {
      node.visitChildren(this);
    }
  }

  @override
  void visitInstanceConstantReference(InstanceConstant node) {
    super.visitInstanceConstantReference(node);
    annotateConstantClass(node.classNode);
  }

  void annotateClass(Class node) {
    if (annotatedClasses.add(node)) {
      node.addAnnotation(ConstantExpression(pragma));
    }
  }

  void annotateMember(Member node) {
    if (annotatedMembers.add(node)) {
      node.addAnnotation(ConstantExpression(pragma));
    }
  }

  // Annotate class potentially used in an InstanceConstant.
  void annotateConstantClass(Class node) {
    if (_annotatedConstantClasses.add(node)) {
      annotateClass(node);
      for (final f in node.fields) {
        if (f.isInstanceMember) {
          annotateMember(f);
          f.initializer?.accept(this);
        }
      }
      final superclass = node.superclass;
      if (superclass != null) {
        annotateConstantClass(superclass);
      }
    }
  }

  /// Annotate all types mentioned in the chain of supertypes of the given
  /// class as they can be referenced by a dynamic module when allocating
  /// an instance of the class.
  void annotateSuperTypes(Class node) {
    if (_annotatedSuperTypes.add(node)) {
      final supertype = node.supertype;
      if (supertype != null) {
        visitList(supertype.typeArguments, this);
        annotateSuperTypes(supertype.classNode);
      }
    }
  }

  void annotateDispatchTargets(Component component) {
    for (final m in annotatedMembers) {
      if (m.isInstanceMember) {
        _getClassInfo(m.enclosingClass!).addSelector(m);
      }
    }
    for (final lib in component.libraries) {
      for (final classNode in lib.classes) {
        final classInfo = _getClassInfo(classNode);
        classInfo.collectSelectors();
        for (final selector in classInfo.setterSelectors) {
          final dispatchTarget = classInfo.dispatchTargetsSetters[selector];
          if (dispatchTarget != null) {
            annotateMember(dispatchTarget);
          }
        }
        for (final selector in classInfo.nonSetterSelectors) {
          final dispatchTarget = classInfo.dispatchTargetsNonSetters[selector];
          if (dispatchTarget != null) {
            annotateMember(dispatchTarget);
          }
        }
      }
    }
  }

  _ClassInfo _getClassInfo(Class cls) =>
      _classInfos[cls] ??= _createClassInfo(cls);

  _ClassInfo _createClassInfo(Class cls) {
    final superclass = cls.superclass;
    final superclassInfo = superclass != null
        ? _getClassInfo(superclass)
        : null;
    final mixedInClass = cls.mixedInClass;
    final mixedInClassInfo = mixedInClass != null
        ? _getClassInfo(mixedInClass)
        : null;
    final implementedClassInfos = cls.implementedTypes
        .map((sup) => _getClassInfo(sup.classNode))
        .toList();
    return _ClassInfo(
      cls,
      superclassInfo,
      mixedInClassInfo,
      implementedClassInfos,
    );
  }
}

class _ClassInfo {
  final Class classNode;
  final _ClassInfo? superclass;
  final _ClassInfo? mixedInClass;
  final List<_ClassInfo> implementedClasses;

  // Callable selectors.
  final setterSelectors = Set<Name>();
  final nonSetterSelectors = Set<Name>();
  bool collected = false;

  // Cache of dispatch targets.
  late final Map<Name, Member> dispatchTargetsSetters = _initDispatchTargets(
    true,
  );
  late final Map<Name, Member> dispatchTargetsNonSetters = _initDispatchTargets(
    false,
  );

  _ClassInfo(
    this.classNode,
    this.superclass,
    this.mixedInClass,
    this.implementedClasses,
  );

  void addSelector(Member m) {
    if (m.isInstanceMember) {
      if (!_isSetter(m)) {
        nonSetterSelectors.add(m.name);
      }
      if (m.hasSetter) {
        setterSelectors.add(m.name);
      }
    }
  }

  void addAllSelectors(_ClassInfo other) {
    setterSelectors.addAll(other.setterSelectors);
    nonSetterSelectors.addAll(other.nonSetterSelectors);
  }

  void collectSelectors() {
    if (!collected) {
      final superclass = this.superclass;
      if (superclass != null) {
        superclass.collectSelectors();
        addAllSelectors(superclass);
      }
      final mixedInClass = this.mixedInClass;
      if (mixedInClass != null) {
        mixedInClass.collectSelectors();
        addAllSelectors(mixedInClass);
      }
      for (final c in implementedClasses) {
        c.collectSelectors();
        addAllSelectors(c);
      }
      collected = true;
    }
  }

  Map<Name, Member> _initDispatchTargets(bool setters) {
    Map<Name, Member> targets;
    final superclass = this.superclass;
    if (superclass != null) {
      targets = Map.of(
        setters
            ? superclass.dispatchTargetsSetters
            : superclass.dispatchTargetsNonSetters,
      );
    } else {
      targets = {};
    }
    final mixedInClass = this.mixedInClass;
    if (mixedInClass != null) {
      targets.addAll(
        setters
            ? mixedInClass.dispatchTargetsSetters
            : mixedInClass.dispatchTargetsNonSetters,
      );
    }
    for (Field f in classNode.fields) {
      if (!f.isStatic && !f.isAbstract) {
        if (!setters || f.hasSetter) {
          targets[f.name] = f;
        }
      }
    }
    for (Procedure p in classNode.procedures) {
      if (!p.isStatic && !p.isAbstract) {
        if (p.isSetter == setters) {
          targets[p.name] = p;
        }
      }
    }
    return targets;
  }

  static bool _isSetter(Member m) => m is Procedure && m.isSetter;
}

void discoverLanguageImplPragmasInCoreLibraries(
  DynamicInterfaceSpecification spec,
  Component component,
  CoreTypes coreTypes,
) {
  final languageImplPragmas = DynamicInterfaceLanguageImplPragmas(coreTypes);
  final visitor = _DiscoverLanguageImplPragmasVisitor(
    spec,
    languageImplPragmas,
  );
  for (final lib in component.libraries) {
    if (languageImplPragmas.isPlatformLibrary(lib)) {
      visitor.visitLibrary(lib);
    }
  }
}

class _DiscoverLanguageImplPragmasVisitor extends RecursiveVisitor {
  final DynamicInterfaceSpecification spec;
  final DynamicInterfaceLanguageImplPragmas languageImplPragmas;

  _DiscoverLanguageImplPragmasVisitor(this.spec, this.languageImplPragmas);

  @override
  void visitClass(Class node) {
    if (languageImplPragmas.isExtendable(node)) {
      spec.extendable.add(node);
    }
    if (languageImplPragmas.isCallable(node)) {
      spec.callable.add(node);
    }
    if (languageImplPragmas.canBeUsedAsType(node)) {
      spec.canBeUsedAsType.add(node);
    }
    node.visitChildren(this);
  }

  @override
  void visitExtensionTypeDeclaration(ExtensionTypeDeclaration node) {
    if (languageImplPragmas.isCallable(node)) {
      spec.callable.add(node);
    }
    if (languageImplPragmas.canBeUsedAsType(node)) {
      spec.canBeUsedAsType.add(node);
    }
    node.visitChildren(this);
  }

  @override
  void defaultMember(Member node) {
    if (languageImplPragmas.isCallable(node)) {
      spec.callable.add(node);
    }
    if (languageImplPragmas.canBeOverridden(node)) {
      if (!node.isInstanceMember) {
        throw 'Expected instance member $node';
      }
      spec.canBeOverridden.add(node);
    }
  }
}

// In a mixin application, clones of instance members always override
// all members of the original mixin. So declaring a mixin as extendable
// implies that all its members can be overridden.
void annotateMembersOfExtendableMixinsAsCanBeOverridden(
  Set<Class> extendableClasses,
  Set<Member> overriddenMembers,
  Constant pragma,
) {
  for (final cls in extendableClasses) {
    if (cls.isMixinClass || cls.isMixinDeclaration) {
      for (final member in cls.members) {
        if (member.isInstanceMember) {
          if (overriddenMembers.add(member)) {
            member.addAnnotation(ConstantExpression(pragma));
          }
        }
      }
    }
  }
}

// Mixin transformation copies all members of mixins into mixin applications.
// So we need to copy can-be-overridden pragmas from members of mixins to
// their copies in the transformed mixin applications.
void copyCanBeOverriddenToMixinApplications(
  Set<Member> overriddenMembers,
  Constant pragma,
  Component component,
  ClassHierarchy hierarchy,
) {
  void processMember(Member original, Class mixinApplication) {
    if (!original.isInstanceMember) {
      return;
    }
    if (!overriddenMembers.contains(original)) {
      return;
    }
    final member = ClassHierarchy.findMemberByName(
      hierarchy.getDeclaredMembers(
        mixinApplication,
        setters: original is Procedure && original.isSetter,
      ),
      original.name,
    );
    if (member == null) {
      return;
    }
    if (overriddenMembers.add(member)) {
      member.addAnnotation(ConstantExpression(pragma));
    }
  }

  for (final library in component.libraries) {
    for (final cls in library.classes) {
      if (cls.isEliminatedMixin) {
        final origin = cls.implementedTypes.last.classNode;
        for (final proc in origin.procedures) {
          processMember(proc, cls);
        }
        for (final field in origin.fields) {
          processMember(field, cls);
        }
      }
    }
  }
}

class _DetailedDynamicInterfaceLogger {
  final Map<String, List<Map<String, String>>> json;
  late List<Map<String, String>> section;

  _DetailedDynamicInterfaceLogger(this.json);

  void setActiveSection(String name) {
    section = (json[name] ??= <Map<String, String>>[]);
  }

  void logNode(TreeNode node) {
    switch (node) {
      case Class():
        section.add({
          'library': node.enclosingLibrary.importUri.toString(),
          'class': node.name,
        });
      case ExtensionTypeDeclaration():
        section.add({
          'library': node.enclosingLibrary.importUri.toString(),
          'extension_type': node.name,
        });
      case Member() when node.enclosingClass != null:
        section.add({
          'library': node.enclosingLibrary.importUri.toString(),
          'class': node.enclosingClass!.demangledName,
          'member': node.name.text,
        });
      case Member() when node.enclosingClass == null:
        section.add({
          'library': node.enclosingLibrary.importUri.toString(),
          'member': node.name.text,
        });
    }
  }
}
