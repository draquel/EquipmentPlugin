#include "EquipmentGASSettings.h"
#include "GameplayEffect.h"

UEquipmentGASSettings::UEquipmentGASSettings()
{
}

TSubclassOf<UGameplayEffect> UEquipmentGASSettings::ResolveStatModifierEffectClass()
{
	const UEquipmentGASSettings* Settings = GetDefault<UEquipmentGASSettings>();
	if (!Settings || !Settings->StatModifierEffectClass.IsValid())
	{
		return nullptr;
	}
	UClass* Loaded = Settings->StatModifierEffectClass.TryLoadClass<UGameplayEffect>();
	return Loaded ? TSubclassOf<UGameplayEffect>(Loaded) : nullptr;
}
