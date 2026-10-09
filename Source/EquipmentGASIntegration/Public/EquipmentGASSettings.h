#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "EquipmentGASSettings.generated.h"

class UGameplayEffect;

/**
 * Project settings for the equipment GAS integration (Project Settings > Plugins > Equipment GAS).
 *
 * StatModifierEffectClass is the duration/infinite gameplay effect through which
 * UItemFragment_Equipment::StatModifiers are applied. Its modifiers must be SetByCaller keyed by
 * SetByCaller.Stat.<AttributeName> (see UCGFGameplayEffectStatics::ApplyStatModifierEffect); the
 * game provides it because only the game knows its attribute sets. Unset = StatModifiers are
 * ignored with a warning.
 */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "Equipment GAS"))
class EQUIPMENTGASINTEGRATION_API UEquipmentGASSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UEquipmentGASSettings();

	/** Effect class used for data-driven equipment stats. */
	UPROPERTY(Config, EditAnywhere, Category = "Stats", meta = (MetaClass = "/Script/GameplayAbilities.GameplayEffect"))
	FSoftClassPath StatModifierEffectClass;

	/** Loaded StatModifierEffectClass, or null when unset / unresolvable. */
	static TSubclassOf<UGameplayEffect> ResolveStatModifierEffectClass();

	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }
};
