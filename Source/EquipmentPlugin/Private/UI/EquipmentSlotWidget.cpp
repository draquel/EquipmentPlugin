// Copyright Daniel Raquel. All Rights Reserved.

#include "UI/EquipmentSlotWidget.h"
#include "Components/EquipmentManagerComponent.h"
#include "Types/EquipmentSystemTypes.h"
#include "Types/CGFItemTypes.h"
#include "Data/ItemDefinition.h"
#include "Subsystems/ItemDatabaseSubsystem.h"
#include "Components/SizeBox.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/Image.h"
#include "Components/TextBlock.h"
#include "Engine/StreamableManager.h"
#include "Engine/AssetManager.h"
#include "Engine/Texture2D.h"
#include "Blueprint/WidgetTree.h"

void UEquipmentSlotWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	BuildWidgetTree();
}

void UEquipmentSlotWidget::NativeDestruct()
{
	if (BoundEquipmentManager)
	{
		BoundEquipmentManager->OnEquipmentChanged.RemoveDynamic(this, &UEquipmentSlotWidget::HandleEquipmentChanged);
	}

	if (IconLoadHandle.IsValid())
	{
		IconLoadHandle->CancelHandle();
		IconLoadHandle.Reset();
	}

	Super::NativeDestruct();
}

void UEquipmentSlotWidget::BuildWidgetTree()
{
	if (!WidgetTree)
	{
		return;
	}

	// Root: Vertical box (icon area + label)
	UVerticalBox* VBox = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("SlotVBox"));
	WidgetTree->RootWidget = VBox;

	// Size box for icon area
	RootSizeBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), TEXT("SlotSizeBox"));
	RootSizeBox->SetWidthOverride(SlotSize);
	RootSizeBox->SetHeightOverride(SlotSize);
	UVerticalBoxSlot* SizeBoxSlot = VBox->AddChildToVerticalBox(RootSizeBox);
	if (SizeBoxSlot)
	{
		SizeBoxSlot->SetHorizontalAlignment(HAlign_Center);
	}

	// Overlay: background + icon
	UOverlay* Overlay = WidgetTree->ConstructWidget<UOverlay>(UOverlay::StaticClass(), TEXT("SlotOverlay"));
	RootSizeBox->AddChild(Overlay);

	BackgroundImage = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("BgImage"));
	if (SlotBackgroundBrush.HasUObject() || SlotBackgroundBrush.GetResourceName() != NAME_None)
	{
		BackgroundImage->SetBrush(SlotBackgroundBrush);
	}
	else
	{
		BackgroundImage->SetColorAndOpacity(FLinearColor(0.08f, 0.08f, 0.12f, 0.9f));
	}
	UOverlaySlot* BgSlot = Overlay->AddChildToOverlay(BackgroundImage);
	if (BgSlot)
	{
		BgSlot->SetHorizontalAlignment(HAlign_Fill);
		BgSlot->SetVerticalAlignment(VAlign_Fill);
	}

	IconImage = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("IconImage"));
	IconImage->SetVisibility(ESlateVisibility::Collapsed);
	UOverlaySlot* IconSlot = Overlay->AddChildToOverlay(IconImage);
	if (IconSlot)
	{
		IconSlot->SetHorizontalAlignment(HAlign_Center);
		IconSlot->SetVerticalAlignment(VAlign_Center);
	}

	// Slot name label
	SlotNameText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("SlotNameText"));
	SlotNameText->SetText(FText::GetEmpty());
	FSlateFontInfo SmallFont = SlotNameText->GetFont();
	SmallFont.Size = 10;
	SlotNameText->SetFont(SmallFont);
	SlotNameText->SetColorAndOpacity(FSlateColor(FLinearColor(0.8f, 0.8f, 0.8f, 1.f)));
	UVerticalBoxSlot* NameSlot = VBox->AddChildToVerticalBox(SlotNameText);
	if (NameSlot)
	{
		NameSlot->SetHorizontalAlignment(HAlign_Center);
		NameSlot->SetPadding(FMargin(0.f, 2.f, 0.f, 0.f));
	}
}

void UEquipmentSlotWidget::InitSlot(UEquipmentManagerComponent* InEquipmentManager, FGameplayTag InSlotTag)
{
	// Unbind old
	if (BoundEquipmentManager)
	{
		BoundEquipmentManager->OnEquipmentChanged.RemoveDynamic(this, &UEquipmentSlotWidget::HandleEquipmentChanged);
	}

	BoundEquipmentManager = InEquipmentManager;
	SlotTag = InSlotTag;

	// Bind new
	if (BoundEquipmentManager)
	{
		BoundEquipmentManager->OnEquipmentChanged.AddDynamic(this, &UEquipmentSlotWidget::HandleEquipmentChanged);
	}

	// Set slot display name from definition
	if (BoundEquipmentManager && SlotNameText)
	{
		for (const FEquipmentSlotDefinition& Def : BoundEquipmentManager->AvailableSlots)
		{
			if (Def.SlotTag == SlotTag)
			{
				SlotNameText->SetText(Def.SlotDisplayName);
				break;
			}
		}
	}

	RefreshSlot();
}

void UEquipmentSlotWidget::RefreshSlot()
{
	if (!BoundEquipmentManager || !SlotTag.IsValid())
	{
		if (IconImage)
		{
			IconImage->SetBrush(EmptySlotBrush);
		}
		return;
	}

	const FItemInstance EquippedItem = BoundEquipmentManager->GetEquippedItem(SlotTag);
	if (!EquippedItem.IsValid())
	{
		if (IconImage)
		{
			IconImage->SetBrush(EmptySlotBrush);
		}
		return;
	}

	// Resolve definition for icon
	UItemDatabaseSubsystem* ItemDB = nullptr;
	if (const UGameInstance* GI = GetWorld() ? GetWorld()->GetGameInstance() : nullptr)
	{
		ItemDB = GI->GetSubsystem<UItemDatabaseSubsystem>();
	}

	if (!ItemDB)
	{
		return;
	}

	const UItemDefinition* Def = ItemDB->GetDefinition(EquippedItem.ItemDefinitionId);
	if (!Def || Def->Icon.IsNull())
	{
		return;
	}

	if (Def->Icon.IsValid())
	{
		if (IconImage)
		{
			IconImage->SetBrushFromTexture(Def->Icon.Get());
		}
	}
	else
	{
		if (IconLoadHandle.IsValid())
		{
			IconLoadHandle->CancelHandle();
		}

		TSoftObjectPtr<UTexture2D> IconRef = Def->Icon;
		IconLoadHandle = UAssetManager::GetStreamableManager().RequestAsyncLoad(
			IconRef.ToSoftObjectPath(),
			FStreamableDelegate::CreateWeakLambda(this, [this, IconRef]()
			{
				if (IconImage && IconRef.IsValid())
				{
					IconImage->SetBrushFromTexture(IconRef.Get());
				}
			})
		);
	}
}

void UEquipmentSlotWidget::HandleEquipmentChanged()
{
	RefreshSlot();
}
