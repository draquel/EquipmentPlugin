using UnrealBuildTool;

public class EquipmentGASIntegration : ModuleRules
{
	public EquipmentGASIntegration(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"GameplayTags",
			"GameplayAbilities",
			"GameplayTasks",
			"CommonGameFramework",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"EquipmentPlugin",
			"ItemInventoryPlugin",
		});
	}
}
