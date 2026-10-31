// Copyright (c) 2024, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import 'dart:async';
import 'dart:io' as io show exitCode, File, IOSink;

import 'package:args/args.dart' show ArgParser, ArgResults;
import 'package:front_end/src/api_unstable/vm.dart'
    show
        CompilerOptions,
        InvocationMode,
        CfeDiagnosticMessage,
        Verbosity,
        parseExperimentalArguments,
        parseExperimentalFlags,
        resolveInputUri;
import 'package:kernel/ast.dart' show Component;
import 'package:vm/bytecode/full_replacement.dart';
import 'package:vm/native_assets/metadata.dart';
import 'package:vm/target_os.dart';
import 'package:vm/kernel_front_end.dart'
    show
        badUsageExitCode,
        compileTimeErrorExitCode,
        compileToKernel,
        convertToPackageUri,
        createFrontEndFileSystem,
        createFrontEndTarget,
        ErrorDetector,
        ErrorPrinter,
        KernelCompilationArguments,
        parseCommandLineDefines,
        successExitCode,
        writeDepfile;
import 'package:vm/transformations/prefix_library_uris.dart'
    as prefix_library_uris;

import 'bytecode_serialization.dart' show BytecodeSizeStatistics;
import 'bytecode_generator.dart' show generateBytecode;
import 'full_replacement.dart';
import 'options.dart' show BytecodeOptions;
import 'patch.dart' show createBytecodePatch;

final ArgParser _argParser = ArgParser(allowTrailingOptions: true)
  ..addOption(
    'platform',
    help: 'Path to vm_platform.dill file',
    defaultsTo: null,
  )
  ..addOption(
    'packages',
    help: 'Path to .dart_tool/package_config.json file',
    defaultsTo: null,
  )
  ..addOption(
    'output',
    abbr: 'o',
    help: 'Path to resulting bytecode file',
    defaultsTo: null,
  )
  ..addOption('depfile', help: 'Path to output Ninja depfile')
  ..addMultiOption(
    'source',
    help: 'List additional source files to include into compilation.',
    defaultsTo: const <String>[],
  )
  ..addOption(
    'target-os',
    help: 'Compile for a specific target operating system.',
    allowed: TargetOS.names,
  )
  ..addOption(
    'patch-base',
    help: 'Full release bytecode baseline; emit a compatible bytecode delta',
  )
  ..addFlag(
    'shorebird-structural-patching',
    help:
        'Enable the explicit structural patch profile for a full baseline or '
        'delta. Requires a matching native structural baseline.',
    defaultsTo: false,
  )
  ..addFlag(
    'shorebird-full-replacement',
    defaultsTo: false,
    help: 'Emit a complete mutable-domain replacement wrapper.',
  )
  ..addOption(
    'shorebird-host-abi',
    help: 'Frozen host ABI SHA-256, 64 lowercase hex digits.',
  )
  ..addOption(
    'depfile-target',
    help: 'Override the target in the generated depfile',
    hide: true,
  )
  ..addMultiOption(
    'filesystem-root',
    help:
        'A base path for the multi-root virtual file system.'
        ' If multi-root file system is used, the input script and .dart_tool/package_config.json file should be specified using URI.',
  )
  ..addOption(
    'filesystem-scheme',
    help: 'The URI scheme for the multi-root virtual filesystem.',
  )
  ..addOption(
    'target',
    help: 'Target model that determines what core libraries are available',
    allowed: <String>['vm', 'flutter', 'flutter_runner', 'dart_runner'],
    defaultsTo: 'vm',
  )
  ..addMultiOption(
    'define',
    abbr: 'D',
    help: 'The values for the environment constants (e.g. -Dkey=value).',
  )
  ..addOption(
    'import-dill',
    help: 'Import libraries from existing dill file',
    defaultsTo: null,
  )
  ..addFlag(
    'enable-asserts',
    help: 'Whether asserts will be enabled.',
    defaultsTo: false,
  )
  ..addMultiOption(
    'bytecode-options',
    help: 'Specify options for bytecode generation:',
    valueHelp: 'opt1,opt2,...',
    allowed: BytecodeOptions.commandLineFlags.keys,
    allowedHelp: BytecodeOptions.commandLineFlags,
  )
  ..addMultiOption(
    'enable-experiment',
    help: 'Comma separated list of experimental features to enable.',
  )
  ..addFlag(
    'help',
    abbr: 'h',
    negatable: false,
    help: 'Print this help message.',
  )
  ..addFlag(
    'track-creation-locations',
    help:
        'Run a kernel transformer to track creation locations for'
        ' classes annotated with @pragma(\'track-creation-locations\').',
    defaultsTo: false,
    aliases: [
      // TODO(http://dartbug.com/63225): Remove this once flutter is migrated
      // to the new flag.
      'track-widget-creation',
    ],
  )
  ..addOption(
    'invocation-modes',
    help: 'Provides information to the front end about how it is invoked.',
    defaultsTo: '',
  )
  ..addOption(
    'validate',
    help:
        'Validate dynamic module against specified dynamic interface YAML file',
    defaultsTo: null,
  )
  ..addOption(
    'verbosity',
    help:
        'Sets the verbosity level used for filtering messages during '
        'compilation.',
    defaultsTo: Verbosity.defaultValue,
  )
  ..addOption(
    'prefix-library-uris',
    help: 'Slash-separated prefix to add to all library uris',
    defaultsTo: '',
  )
  ..addFlag(
    'allow-dynamic-calls-in-dynamic-modules',
    help: 'Allow dynamic calls in dynamic modules',
    defaultsTo: false,
  )
  ..addMultiOption(
    'extra-selectors-allowed-in-dynamic-calls',
    help:
        'Selector names that weren\'t exposed by the host, but are allowed in '
        'dynamic calls within the dynamic module. Pass "*" to allow all dynamic '
        'calls, which is unsafe and may result in runtime errors if target of a '
        'dynamic call is not from a dynamic module and not listed as '
        'dynamically-callable in the dynamic interface.',
  );

final String _usage =
    '''
Usage: dart2bytecode --platform vm_platform.dill [--import-dill host_app.dill] [--validate dynamic_interface.yaml] [options] input.dart
Compiles Dart sources to Dart bytecode.

Options:
${_argParser.usage}
''';

Future<void> main(List<String> arguments) async {
  io.exitCode = await runCompilerWithCommandLineArguments(arguments);
}

/// Run bytecode compiler tool with given [arguments]
/// and return exit code (0 on success, non-zero on failure).
Future<int> runCompilerWithCommandLineArguments(List<String> arguments) async {
  final ArgResults options = _argParser.parse(arguments);
  final String? platformKernel = options['platform'];

  if (options['help']) {
    print(_usage);
    return successExitCode;
  }

  final String? input = options.rest.singleOrNull;
  if (input == null || platformKernel == null) {
    print(_usage);
    return badUsageExitCode;
  }

  final String outputFileName = options['output'] ?? "$input.bytecode";
  final String? packages = options['packages'];
  final String targetName = options['target'];
  final String? fileSystemScheme = options['filesystem-scheme'];
  final String? depfile = options['depfile'];
  final String? depfileTarget = options['depfile-target'];
  final List<String>? fileSystemRoots = options['filesystem-root'];
  final bool enableAsserts = options['enable-asserts'];
  final List<String>? experimentalFlags = options['enable-experiment'];
  final Map<String, String> environmentDefines = {};

  if (!parseCommandLineDefines(options['define'], environmentDefines, _usage)) {
    return badUsageExitCode;
  }

  final String? importDill = options['import-dill'];
  final String? validateDynamicInterface = options['validate'];
  final String messageVerbosity = options['verbosity'];
  final String cfeInvocationModes = options['invocation-modes'];
  final bool trackCreationLocations = options['track-creation-locations'];
  final List<String>? bytecodeGeneratorOptions = options['bytecode-options'];
  final String libraryUrisPrefix = options['prefix-library-uris']!;
  final bool allowDynamicCallsInDynamicModules =
      options['allow-dynamic-calls-in-dynamic-modules'];
  final List<String> dynamicCallsSelectorAllowList =
      options['extra-selectors-allowed-in-dynamic-calls'];

  return await runCompilerWithOptions(
    input: input,
    additionalSources: options['source'],
    platformKernel: platformKernel,
    outputFileName: outputFileName,
    targetName: targetName,
    targetOS: options['target-os'],
    packages: packages,
    importDill: importDill,
    validateDynamicInterface: validateDynamicInterface,
    enableAsserts: enableAsserts,
    experimentalFlags: experimentalFlags,
    environmentDefines: environmentDefines,
    fileSystemScheme: fileSystemScheme,
    fileSystemRoots: fileSystemRoots,
    messageVerbosity: messageVerbosity,
    cfeInvocationModes: cfeInvocationModes,
    trackCreationLocations: trackCreationLocations,
    bytecodeGeneratorOptions: bytecodeGeneratorOptions,
    depfile: depfile,
    depfileTarget: depfileTarget,
    libraryUrisPrefix: libraryUrisPrefix,
    allowDynamicCallsInDynamicModules: allowDynamicCallsInDynamicModules,
    dynamicCallsSelectorAllowList: dynamicCallsSelectorAllowList,
    patchBase: options['patch-base'],
    shorebirdStructuralPatching: options['shorebird-structural-patching'],
    shorebirdFullReplacement: options['shorebird-full-replacement'],
    shorebirdHostAbi: options['shorebird-host-abi'],
  );
}

/// Run bytecode compiler tool with given options
/// and return exit code (0 on success, non-zero on failure).
///
/// [additionalSources] adds source roots using the same path and URI resolution
/// as the native kernel compiler. [input] remains the program root.
Future<int> runCompilerWithOptions({
  required String input,
  required String platformKernel,
  required String outputFileName,
  required String targetName,
  List<String> additionalSources = const [],
  String? packages,
  String? importDill,
  String? validateDynamicInterface,
  bool enableAsserts = false,
  List<String>? experimentalFlags,
  Map<String, String> environmentDefines = const {},
  String? fileSystemScheme,
  List<String>? fileSystemRoots,
  String messageVerbosity = Verbosity.defaultValue,
  void Function(String) printMessage = print,
  String cfeInvocationModes = '',
  bool trackCreationLocations = false,
  List<String>? bytecodeGeneratorOptions,
  String? depfile,
  String? depfileTarget,
  required String libraryUrisPrefix,
  bool allowDynamicCallsInDynamicModules = false,
  List<String> dynamicCallsSelectorAllowList = const [],
  String? patchBase,
  bool shorebirdStructuralPatching = false,
  bool shorebirdFullReplacement = false,
  String? shorebirdHostAbi,
  String? targetOS,
}) async {
  if (shorebirdFullReplacement && shorebirdStructuralPatching) {
    printMessage(
      '--shorebird-full-replacement and --shorebird-structural-patching are mutually exclusive.',
    );
    return badUsageExitCode;
  }
  if (targetOS != null && TargetOS.fromString(targetOS) == null) {
    printMessage('Unsupported target operating system: $targetOS.');
    return badUsageExitCode;
  }
  if (shorebirdFullReplacement &&
      targetName == 'flutter' &&
      targetOS != 'ios') {
    printMessage('Flutter full replacement requires --target-os=ios.');
    return badUsageExitCode;
  }
  if (shorebirdFullReplacement || shorebirdHostAbi != null) {
    try {
      if (!shorebirdFullReplacement || shorebirdHostAbi == null) {
        throw const FormatException(
          '--shorebird-full-replacement requires --shorebird-host-abi.',
        );
      }
      parseShorebirdHostAbi(shorebirdHostAbi);
    } on FormatException catch (error) {
      printMessage(error.message);
      return badUsageExitCode;
    }
  }
  final fileSystem = createFrontEndFileSystem(
    fileSystemScheme,
    fileSystemRoots,
  );

  final Uri? packagesUri = packages != null ? resolveInputUri(packages) : null;

  final platformKernelUri = Uri.base.resolveUri(new Uri.file(platformKernel));

  final List<Uri> additionalDillModules = <Uri>[];
  if (importDill != null) {
    additionalDillModules.add(Uri.base.resolveUri(new Uri.file(importDill)));
  }

  final Uri? dynamicInterfaceSpecificationUri =
      (validateDynamicInterface != null)
      ? resolveInputUri(validateDynamicInterface)
      : null;

  final verbosity = Verbosity.parseArgument(messageVerbosity);
  final errorPrinter = ErrorPrinter(verbosity, println: printMessage);
  final errorDetector = ErrorDetector(previousErrorHandler: errorPrinter.call);

  Uri mainUri = resolveInputUri(input);
  if (packagesUri != null) {
    mainUri = await convertToPackageUri(fileSystem, mainUri, packagesUri);
  }

  final BytecodeOptions bytecodeOptions = BytecodeOptions(
    enableAsserts: enableAsserts,
    shorebirdStructuralPatching: shorebirdStructuralPatching,
    shorebirdFullReplacement: shorebirdFullReplacement,
  )..parseCommandLineFlags(bytecodeGeneratorOptions);

  final CompilerOptions compilerOptions = CompilerOptions()
    ..sdkSummary = platformKernelUri
    ..fileSystem = fileSystem
    ..additionalDillModules = additionalDillModules
    ..packagesFileUri = packagesUri
    ..dynamicInterfaceSpecificationUri = dynamicInterfaceSpecificationUri
    ..allowDynamicCallsInDynamicModules = allowDynamicCallsInDynamicModules
    ..dynamicCallsSelectorAllowList = dynamicCallsSelectorAllowList
    ..explicitExperimentalFlags = parseExperimentalFlags(
      parseExperimentalArguments(experimentalFlags),
      onError: printMessage,
    )
    ..onDiagnostic = (CfeDiagnosticMessage m) {
      errorDetector(m);
    }
    ..embedSourceText = bytecodeOptions.embedSourceText
    ..invocationModes = InvocationMode.parseArguments(cfeInvocationModes)
    ..verbosity = verbosity
    ..target = createFrontEndTarget(
      targetName,
      trackCreationLocations: trackCreationLocations,
      supportMirrors: false,
      shorebirdIosFullReplacement:
          shorebirdFullReplacement &&
          targetName == 'flutter' &&
          targetOS == 'ios',
      isClosureContextLoweringEnabled:
          bytecodeOptions.isClosureContextLoweringEnabled,
    );

  if (compilerOptions.target == null) {
    printMessage('Failed to create front-end target $targetName.');
    return badUsageExitCode;
  }

  final results = await compileToKernel(
    KernelCompilationArguments(
      source: mainUri,
      additionalSources: additionalSources.map(resolveInputUri).toList(),
      options: compilerOptions,
      requireMain: false,
      includePlatform: false,
      environmentDefines: Map.of(environmentDefines),
      enableAsserts: enableAsserts,
      targetOS: targetOS,
    ),
  );

  errorPrinter.printCompilationMessages();

  Component? component = results.component;
  if (errorDetector.hasCompilationErrors || component == null) {
    return compileTimeErrorExitCode;
  }
  component = prefix_library_uris.prefixLibraryUris(
    component,
    results.loadedLibraries,
    libraryUrisPrefix,
  );
  if (bytecodeOptions.showBytecodeSizeStatistics) {
    BytecodeSizeStatistics.reset();
  }
  if (shorebirdFullReplacement) {
    try {
      validateShorebirdReplacementLibraries(
        component.libraries.where(
          (library) => !isNativeAssetsMetadataLibrary(library),
        ),
      );
    } on FormatException catch (error) {
      printMessage(error.message);
      return compileTimeErrorExitCode;
    }
  }
  final io.IOSink sink = io.File(outputFileName).openWrite();
  generateBytecode(
    component,
    sink,
    libraries: component.libraries
        .where(
          (lib) => shorebirdFullReplacement
              ? lib.importUri.scheme != 'dart' &&
                    !isNativeAssetsMetadataLibrary(lib)
              : !results.loadedLibraries.contains(lib),
        )
        .toList(),
    hierarchy: results.classHierarchy!,
    coreTypes: results.coreTypes!,
    options: bytecodeOptions,
    target: compilerOptions.target!,
    extraLoadedLibraries: results.loadedLibraries,
  );
  await sink.close();
  if (patchBase != null || shorebirdFullReplacement) {
    final output = io.File(outputFileName);
    try {
      final updated = await output.readAsBytes();
      final delta = shorebirdFullReplacement
          ? createShorebirdFullReplacement(
              updated,
              hostAbi: parseShorebirdHostAbi(shorebirdHostAbi!),
              rootUri:
                  (component.mainMethod?.enclosingLibrary ??
                          component.libraries.firstWhere(
                            (library) => library.importUri == mainUri,
                          ))
                      .importUri
                      .toString(),
              baseline: patchBase == null
                  ? null
                  : await io.File(patchBase).readAsBytes(),
            )
          : createBytecodePatch(
              await io.File(patchBase!).readAsBytes(),
              updated,
              shorebirdStructuralPatching: shorebirdStructuralPatching,
            );
      await output.writeAsBytes(delta);
    } on FormatException catch (error) {
      await output.delete();
      printMessage(error.message);
      return compileTimeErrorExitCode;
    } catch (_) {
      // Never leave a full component at the requested delta output path when
      // reading the baseline or constructing the delta failed.
      await output.delete();
      rethrow;
    }
  }
  if (bytecodeOptions.showBytecodeSizeStatistics) {
    BytecodeSizeStatistics.dump();
  }

  if (depfile != null) {
    await writeDepfile(
      fileSystem,
      results.compiledSources!,
      depfileTarget ?? outputFileName,
      depfile,
    );
  }

  return successExitCode;
}
