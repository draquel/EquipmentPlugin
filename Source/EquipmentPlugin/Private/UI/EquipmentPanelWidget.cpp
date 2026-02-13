// Copyright Daniel Raquel. All Rights Reserved.

#include "UI/EquipmentPanelWidget.h"
#include "UI/EquipmentSlotWidget.h"
#include "Components/EquipmentManagerComponent.h"
#include "Types/EquipmentSystemTypes.h"
#include "Components/Border.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/TextBlock.h"
#include "Blueprint/WidgetTree.h"

void UEquipmentPanelWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	BuildWidgetTree();
}

void UEquipmentPanelWidget::BuildWidgetTree()
{
	if (!WidgetTree)
	{
		return;
	}

	// Root: UBorder
	RootBorder = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("PanelBorder"));
	RootBorder->SetBrush(PanelBackgroundBrush);
	RootBorder->SetBrushColor(PanelBackgroundTint);
	RootBorder->SetPadding(PanelPadding);
	WidgetTree->RootWidget = RootBorder;

	// Vertical box: title + slot container
	UVerticalBox* VBox = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("PanelVBox"));
	RootBorder->AddChild(VBox);

	// Title
	TitleText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("PanelTitle"));
	TitleText->SetText(PanelTitle);
	FSlateFontInfo TitleFont = TitleText->GetFont();
	TitleFont.Size = 16;
	TitleText->SetFont(TitleFont);
	TitleText->SetColorAndOpacity(FSlateColor(FLinearColor::White));
	UVerticalBoxSlot* TitleSlot = VBox->AddChildToVerticalBox(TitleText);
	if (TitleSlot)
	{
		TitleSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 4.f));
	}

	// Slot container
	SlotContainer = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("SlotContainer"));
	VBox->AddChildToVerticalBox(SlotContainer);
}

void UEquipmentPanelWidget::InitPanel(UEquipmentManagerComponent* InEquipmentManager)
{
	BoundEquipmentManager = InEquipmentManager;

	if (!SlotContainer)
	{
		return;
	}

	// Clear existing
	SlotContainer->ClearChildren();
	SlotWidgets.Empty();

	if (!BoundEquipmentManager)
	{
		return;
	}

	TSubclassOf<UEquipmentSlotWidget> ClassToUse = SlotWidgetClass;
	if (!ClassToUse)
	{
		ClassToUse = UEquipmentSlotWidget::StaticClass();
	}

	for (const FEquipmentSlotDefinition& SlotDef : BoundEquipmentManager->AvailableSlots)
	{
		UEquipmentSlotWidget* SlotWidget = CreateWidget<UEquipmentSlotWidget>(GetOwningPlayer(), ClassToUse);
		if (SlotWidget)
		{
			SlotWidget->InitSlot(BoundEquipmentManager, SlotDef.SlotTag);

			UVerticalBoxSlot* VBSlot = SlotContainer->AddChildToVerticalBox(SlotWidget);
			if (VBSlot)
			{
				VBSlot->SetPadding(FMargin(0.f, SlotSpacing * 0.5f));
				VBSlot->SetHorizontalAlignment(HAlign_Center);
			}

			SlotWidgets.Add(SlotWidget);
		}
	}
}

void UEquipmentPanelWidget::RefreshAllSlots()
{
	for (UEquipmentSlotWidget* SlotWidget : SlotWidgets)
	{
		if (SlotWidget)
		{
			SlotWidget->RefreshSlot();
		}
	}
}
