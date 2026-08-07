using Mutagen.Bethesda.Fallout4;
using Mutagen.Bethesda.Plugins;

try
{
    var options = Options.Parse(args);
    var masterPath = Path.Combine(options.DataDirectory, "Fallout4.esm");
    if (!File.Exists(masterPath))
    {
        throw new FileNotFoundException(
            $"Fallout4.esm was not found under '{options.DataDirectory}'.",
            masterPath);
    }

    using var fallout4 = Fallout4Mod.Create(Fallout4Release.Fallout4)
        .FromPath(new ModPath(ModKey.FromFileName("Fallout4.esm"), masterPath))
        .WithBsaFolder(options.DataDirectory)
        .Construct();

    if (options.Scan)
    {
        ScanMaster(fallout4);
        return 0;
    }

    var outputPath = Path.GetFullPath(options.OutputPath);
    Directory.CreateDirectory(Path.GetDirectoryName(outputPath)!);

    var output = new Fallout4Mod(
        ModKey.FromFileName(Constants.PluginFileName),
        Fallout4Release.Fallout4,
        forceUseLowerFormIDRanges: true)
    {
        IsSmallMaster = true,
    };
    output.ModHeader.Stats.NextFormID = Constants.FirstLightPluginFormId;

    BuildPlugin(fallout4, output, options.ProgramSwf);

    output.BeginWrite
        .ToPath(outputPath)
        .WithLoadOrder(fallout4)
        .Write();

    VerifyPlugin(outputPath, options.ProgramSwf);
    Console.WriteLine($"Generated {outputPath}");
    return 0;
}
catch (Exception exception)
{
    Console.Error.WriteLine($"World plugin generation failed: {exception}");
    return 1;
}

static void BuildPlugin(
    IFallout4ModGetter fallout4,
    Fallout4Mod output,
    string programSwf)
{
    var tableTelevision = FindActivatorByModel(
        fallout4,
        "TelevisionWorkshopVariant01.nif");
    var cabinetTelevision = FindActivatorByModel(
        fallout4,
        "TelevisionWorkshopVariant02.nif");
    var projectorModel = FindActivatorByModel(
        fallout4,
        "FilmProjector.nif");
    var screenModel = fallout4.Statics.FirstOrDefault(record =>
        ModelPath(record.Model).EndsWith(
            @"SetDressing\DriveIn\DriveinScreen01.nif",
            StringComparison.OrdinalIgnoreCase))
        ?? throw new InvalidOperationException(
            "The vanilla drive-in screen could not be located.");

    AddWorkshopObject(
        fallout4,
        output,
        tableTelevision,
        "MMVP_WorkshopTelevisionTable",
        "Video Television (Table)",
        @"MMVP\TelevisionWorkshopVariant01.nif",
        tableTelevision.ObjectBounds,
        null);
    AddWorkshopObject(
        fallout4,
        output,
        cabinetTelevision,
        "MMVP_WorkshopTelevisionCabinet",
        "Video Television (Cabinet)",
        @"MMVP\TelevisionWorkshopVariant02.nif",
        cabinetTelevision.ObjectBounds,
        null);
    AddWorkshopObject(
        fallout4,
        output,
        tableTelevision,
        "MMVP_WorkshopMovieProjector",
        "Movie Projector",
        @"MMVP\FilmProjector.nif",
        projectorModel.ObjectBounds,
        [
            new("c_Aluminum", 2),
            new("c_Circuitry", 4),
            new("c_Glass", 2),
            new("c_Gears", 2),
            new("c_Steel", 4),
        ]);
    AddWorkshopObject(
        fallout4,
        output,
        tableTelevision,
        "MMVP_WorkshopMovieScreen",
        "Movie Screen",
        @"MMVP\MovieScreen.nif",
        screenModel.ObjectBounds,
        [
            new("c_Cloth", 6),
            new("c_Steel", 4),
            new("c_Wood", 4),
        ]);

    // Local FormID 808 belonged to the removed craftable terminal. Skip it so
    // the shipped holotape and quest retain 809/80A in existing saves.
    output.ModHeader.Stats.NextFormID = Constants.PlayerHolotapeFormId;
    AddHolotapePlayer(fallout4, output, programSwf);
}

static void AddHolotapePlayer(
    IFallout4ModGetter fallout4,
    Fallout4Mod output,
    string programSwf)
{
    var holotapeTemplate = fallout4.Holotapes.FirstOrDefault(record =>
        string.Equals(
            record.EditorID,
            "MQ206HolotapeRR",
            StringComparison.OrdinalIgnoreCase))
        ?? throw new InvalidOperationException(
            "The vanilla Railroad holotape template could not be located.");
    var holotape = new Holotape(output);
    holotape.DeepCopyIn(
        holotapeTemplate,
        out _,
        new Holotape.TranslationMask(defaultOn: true)
        {
            VirtualMachineAdapter = false,
            Name = false,
            Data = false,
        });
    holotape.EditorID = "MMVP_PlayerHolotape";
    holotape.Name = "Main Menu Video Player";
    holotape.VirtualMachineAdapter = null;
    holotape.Value = 0;
    holotape.Weight = 0.0F;
    holotape.Data = new HolotapeProgram
    {
        File = programSwf,
    };
    output.Holotapes.Add(holotape);
    if (holotape.FormKey.ID != Constants.PlayerHolotapeFormId)
    {
        throw new InvalidOperationException(
            $"The player holotape must retain local FormID " +
            $"{Constants.PlayerHolotapeFormId:X3}, got {holotape.FormKey.ID:X3}.");
    }

    var questScript = new ScriptEntry
    {
        Name = "MMVP:HolotapeQuest",
        Flags = ScriptEntry.Flag.Local,
    };
    var holotapeProperty = new ScriptObjectProperty
    {
        Name = "PlayerHolotape",
        Flags = ScriptProperty.Flag.Edited,
    };
    holotapeProperty.Object.SetTo(holotape.FormKey);
    questScript.Properties.Add(holotapeProperty);

    var questAdapter = new QuestAdapter
    {
        ObjectFormat = 2,
        ExtraBindDataVersion = 3,
    };
    questAdapter.Scripts.Add(questScript);

    var quest = new Quest(output)
    {
        EditorID = "MMVP_PlayerHolotapeQuest",
        Name = "Main Menu Video Player Inventory Setup",
        Data = new QuestData
        {
            Flags = Quest.Flag.StartGameEnabled |
                    Quest.Flag.RunOnce |
                    Quest.Flag.ExcludeFromDialogExport,
            Priority = 0,
            DelayTime = 0.0F,
            Type = Quest.TypeEnum.None,
        },
        VirtualMachineAdapter = questAdapter,
    };
    output.Quests.Add(quest);

    Console.WriteLine(
        $"Holotape player {holotape.FormKey} ({programSwf}), " +
        $"quest {quest.FormKey}");
}

static IActivatorGetter FindActivatorByModel(
    IFallout4ModGetter fallout4,
    string fileName)
{
    return fallout4.Activators
        .FirstOrDefault(record =>
            ModelPath(record.Model).EndsWith(
                fileName,
                StringComparison.OrdinalIgnoreCase))
        ?? throw new InvalidOperationException(
            $"The vanilla activator using '{fileName}' could not be located.");
}

static void AddWorkshopObject(
    IFallout4ModGetter fallout4,
    Fallout4Mod output,
    IActivatorGetter template,
    string editorId,
    string name,
    string modelPath,
    IObjectBoundsGetter objectBounds,
    IReadOnlyList<MaterialCost>? materialCosts)
{
    var templateRecipe = fallout4.ConstructibleObjects
        .FirstOrDefault(record => record.CreatedObject.FormKey == template.FormKey)
        ?? throw new InvalidOperationException(
            $"The vanilla workshop recipe for {template.FormKey} could not be located.");

    var record = new Mutagen.Bethesda.Fallout4.Activator(output);
    record.DeepCopyIn(
        template,
        out _,
        new Mutagen.Bethesda.Fallout4.Activator.TranslationMask(defaultOn: true)
        {
            Name = false,
            ActivateTextOverride = false,
        });
    record.EditorID = editorId;
    record.Name = name;
    if (record.Model is null)
    {
        throw new InvalidOperationException(
            $"The copied activator {template.FormKey} has no model.");
    }
    record.Model.File = modelPath;
    record.ObjectBounds.First = objectBounds.First;
    record.ObjectBounds.Second = objectBounds.Second;
    output.Activators.Add(record);

    var recipe = new ConstructibleObject(output);
    recipe.DeepCopyIn(
        templateRecipe,
        out _,
        new ConstructibleObject.TranslationMask(defaultOn: true)
        {
            Description = false,
        });
    recipe.EditorID = $"MMVP_co_{editorId["MMVP_".Length..]}";
    recipe.CreatedObject.SetTo(record.FormKey);
    if (materialCosts is not null)
    {
        recipe.Components = new();
        foreach (var cost in materialCosts)
        {
            var component = fallout4.Components.FirstOrDefault(candidate =>
                string.Equals(
                    candidate.EditorID,
                    cost.EditorId,
                    StringComparison.OrdinalIgnoreCase))
                ?? throw new InvalidOperationException(
                    $"Could not locate component '{cost.EditorId}'.");
            var entry = new ConstructibleObjectComponent
            {
                Count = cost.Count,
            };
            entry.Component.SetTo(component.FormKey);
            recipe.Components.Add(entry);
        }
    }
    output.ConstructibleObjects.Add(recipe);

    Console.WriteLine(
        $"{template.EditorID} ({template.FormKey}) -> " +
        $"{record.EditorID} ({record.FormKey}), recipe {recipe.FormKey}");
}

static void ScanMaster(IFallout4ModGetter fallout4)
{
    Console.WriteLine("Furniture with television/projector models:");
    foreach (var record in fallout4.Furniture
                 .Where(record => IsRelevantModel(ModelPath(record.Model))))
    {
        Console.WriteLine(
            $"  {record.FormKey} {record.EditorID} | {ModelPath(record.Model)}");
    }

    Console.WriteLine("Activators with television/projector/screen models:");
    foreach (var record in fallout4.Activators
                 .Where(record => IsRelevantModel(ModelPath(record.Model))))
    {
        Console.WriteLine(
            $"  {record.FormKey} {record.EditorID} | {ModelPath(record.Model)}");
    }

    Console.WriteLine("Statics with television/projector/screen models:");
    foreach (var record in fallout4.Statics
                 .Where(record => IsRelevantModel(ModelPath(record.Model))))
    {
        Console.WriteLine(
            $"  {record.FormKey} {record.EditorID} | {ModelPath(record.Model)}");
    }

    var relevantTargets = fallout4.Activators
        .Where(record => IsRelevantModel(ModelPath(record.Model)))
        .Select(record => record.FormKey)
        .Concat(
            fallout4.Statics
                .Where(record => IsRelevantModel(ModelPath(record.Model)))
                .Select(record => record.FormKey))
        .ToHashSet();
    var componentsByFormKey = fallout4.Components
        .ToDictionary(component => component.FormKey);

    Console.WriteLine("Constructible objects for relevant targets:");
    foreach (var recipe in fallout4.ConstructibleObjects
                 .Where(recipe =>
                     recipe.CreatedObject.FormKey is { } key &&
                     relevantTargets.Contains(key)))
    {
        Console.WriteLine(
            $"  {recipe.FormKey} {recipe.EditorID} -> {recipe.CreatedObject.FormKey} " +
            $"workbench={recipe.WorkbenchKeyword.FormKey}");
        if (recipe.Components is not null)
        {
            foreach (var component in recipe.Components)
            {
                componentsByFormKey.TryGetValue(
                    component.Component.FormKey,
                    out var componentRecord);
                Console.WriteLine(
                    $"    component={component.Component.FormKey} " +
                    $"({componentRecord?.EditorID ?? "unknown"}) " +
                    $"count={component.Count}");
            }
        }
    }

    Console.WriteLine("Terminals with indexed Papyrus fragments:");
    foreach (var terminal in fallout4.Terminals
                 .Where(record =>
                     record.VirtualMachineAdapter?.ScriptFragments is not null)
                 .Take(20))
    {
        var adapter = terminal.VirtualMachineAdapter!;
        var fragments = adapter.ScriptFragments!;
        Console.WriteLine(
            $"  {terminal.FormKey} {terminal.EditorID} | " +
            $"menuItems={terminal.MenuItems?.Count ?? 0}, " +
            $"script={fragments.Script.Name}, " +
            $"fragments={fragments.Fragments.Count}");
        foreach (var fragment in fragments.Fragments.Take(8))
        {
            Console.WriteLine(
                $"    index={fragment.FragmentIndex}, " +
                $"unknown={fragment.Unknown}, unknown2={fragment.Unknown2}, " +
                $"script={fragment.ScriptName}, function={fragment.FragmentName}");
        }
    }

    Console.WriteLine("Holotapes backed by terminal records:");
    foreach (var holotape in fallout4.Holotapes
                 .Where(record => record.Data is HolotapeTerminal)
                 .Take(20))
    {
        var data = (HolotapeTerminal)holotape.Data;
        Console.WriteLine(
            $"  {holotape.FormKey} {holotape.EditorID} | " +
            $"terminal={data.Terminal.FormKey}");
    }

    Console.WriteLine("Holotapes backed by program SWFs:");
    foreach (var holotape in fallout4.Holotapes
                 .Where(record => record.Data is HolotapeProgram)
                 .Take(20))
    {
        var data = (HolotapeProgram)holotape.Data;
        Console.WriteLine(
            $"  {holotape.FormKey} {holotape.EditorID} | file={data.File}");
    }
}

static void VerifyPlugin(string path, string programSwf)
{
    using var generated = Fallout4Mod.CreateFromBinaryOverlay(
        path,
        Fallout4Release.Fallout4);

    if (!generated.IsSmallMaster)
    {
        throw new InvalidDataException("The generated plugin is missing its ESL flag.");
    }

    var records = generated.Activators
        .Cast<IFallout4MajorRecordGetter>()
        .Concat(generated.ConstructibleObjects.Cast<IFallout4MajorRecordGetter>())
        .Concat(generated.Terminals.Cast<IFallout4MajorRecordGetter>())
        .Concat(generated.Holotapes.Cast<IFallout4MajorRecordGetter>())
        .Concat(generated.Quests.Cast<IFallout4MajorRecordGetter>())
        .ToArray();
    if (records.Length != Constants.ExpectedRecordCount)
    {
        throw new InvalidDataException(
            $"Expected {Constants.ExpectedRecordCount} records, found {records.Length}.");
    }

    foreach (var record in records)
    {
        if (record.FormKey.ModKey != generated.ModKey ||
            record.FormKey.ID < Constants.FirstLightPluginFormId ||
            record.FormKey.ID > Constants.LastLightPluginFormId)
        {
            throw new InvalidDataException(
                $"Record {record.FormKey} is outside the ESP-FE local FormID range.");
        }
    }

    var expectedMaster = ModKey.FromFileName("Fallout4.esm");
    if (!generated.ModHeader.MasterReferences.Any(master => master.Master == expectedMaster))
    {
        throw new InvalidDataException("The generated plugin does not list Fallout4.esm.");
    }

    if (generated.Terminals.Any() ||
        generated.ConstructibleObjects.Any(record =>
            record.EditorID == "MMVP_co_PlayerTerminal"))
    {
        throw new InvalidDataException(
            "The removed MMVP workshop terminal is still present.");
    }

    var holotape = generated.Holotapes.Single();
    if (holotape.FormKey.ID != Constants.PlayerHolotapeFormId ||
        holotape.Data is not IHolotapeProgramGetter programData ||
        !string.Equals(
            programData.File,
            programSwf,
            StringComparison.OrdinalIgnoreCase))
    {
        throw new InvalidDataException(
            $"The player holotape does not launch '{programSwf}'.");
    }

    var quest = generated.Quests.Single();
    var questScript = quest.VirtualMachineAdapter?.Scripts.SingleOrDefault();
    var holotapeProperty =
        questScript?.Properties.OfType<IScriptObjectPropertyGetter>()
            .SingleOrDefault(property => property.Name == "PlayerHolotape");
    if (quest.Data is null ||
        !quest.Data.Flags.HasFlag(Quest.Flag.StartGameEnabled) ||
        questScript?.Name != "MMVP:HolotapeQuest" ||
        holotapeProperty?.Object.FormKey != holotape.FormKey)
    {
        throw new InvalidDataException(
            "The inventory quest is not configured to grant the player holotape.");
    }

    Console.WriteLine(
        $"Verified ESP-FE: {records.Length} records, local FormIDs " +
        $"{records.Min(record => record.FormKey.ID):X3}-" +
        $"{records.Max(record => record.FormKey.ID):X3}.");
}

static string ModelPath(IModelGetter? model) => model?.File?.ToString() ?? string.Empty;

static bool IsRelevantModel(string path) =>
    path.Contains("television", StringComparison.OrdinalIgnoreCase) ||
    path.Contains("projector", StringComparison.OrdinalIgnoreCase) ||
    path.Contains("driveinscreen", StringComparison.OrdinalIgnoreCase);

internal sealed record Options(
    string DataDirectory,
    string OutputPath,
    bool Scan,
    string ProgramSwf)
{
    public static Options Parse(string[] args)
    {
        string? dataDirectory = null;
        var outputPath = Path.Combine(
            "package",
            "experimental",
            Constants.PluginFileName);
        var scan = false;
        var programSwf = "MMVPBrowser.swf";

        for (var index = 0; index < args.Length; ++index)
        {
            switch (args[index])
            {
                case "--data-dir":
                    dataDirectory = RequireValue(args, ref index, "--data-dir");
                    break;
                case "--output":
                    outputPath = RequireValue(args, ref index, "--output");
                    break;
                case "--scan":
                    scan = true;
                    break;
                case "--program-swf":
                    programSwf = RequireValue(args, ref index, "--program-swf");
                    break;
                case "--help":
                case "-h":
                    PrintHelp();
                    Environment.Exit(0);
                    break;
                default:
                    throw new ArgumentException($"Unknown argument: {args[index]}");
            }
        }

        if (string.IsNullOrWhiteSpace(dataDirectory))
        {
            throw new ArgumentException("--data-dir is required.");
        }
        if (!string.Equals(
                Path.GetExtension(programSwf),
                ".swf",
                StringComparison.OrdinalIgnoreCase) ||
            !string.Equals(
                Path.GetFileName(programSwf),
                programSwf,
                StringComparison.Ordinal))
        {
            throw new ArgumentException(
                "--program-swf must be one bare SWF filename, " +
                "for example MMVPBrowser.swf.");
        }

        return new Options(
            Path.GetFullPath(dataDirectory),
            Path.GetFullPath(outputPath),
            scan,
            programSwf);
    }

    private static string RequireValue(string[] args, ref int index, string option)
    {
        if (++index >= args.Length || string.IsNullOrWhiteSpace(args[index]))
        {
            throw new ArgumentException($"{option} requires a value.");
        }

        return args[index];
    }

    private static void PrintHelp()
    {
        Console.WriteLine(
            """
            MMVP world plugin generator

            Usage:
              dotnet run --project tools/WorldPluginGenerator -- \
                --data-dir "/path/to/Fallout 4/Data" \
                [--output package/experimental/MMVP_WorldScreens.esp] \
                [--program-swf MMVPBrowser.swf]
              dotnet run --project tools/WorldPluginGenerator -- \
                --data-dir "/path/to/Fallout 4/Data" --scan

            The holotape is always a direct SWF program usable through the
            Pip-Boy or any vanilla terminal that supports Load Holotape.
            --program-swf defaults to MMVPBrowser.swf.
            """);
    }
}

internal static class Constants
{
    public const string PluginFileName = "MMVP_WorldScreens.esp";
    public const uint FirstLightPluginFormId = 0x800;
    public const uint LastLightPluginFormId = 0xFFF;
    public const uint PlayerHolotapeFormId = 0x809;
    public const int ExpectedRecordCount = 10;
}

internal sealed record MaterialCost(string EditorId, uint Count);
