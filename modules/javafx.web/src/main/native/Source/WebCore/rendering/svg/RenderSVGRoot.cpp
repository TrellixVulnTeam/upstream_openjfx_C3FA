/*
 * Copyright (C) 2004, 2005, 2007 Nikolas Zimmermann <zimmermann@kde.org>
 * Copyright (C) 2004, 2005, 2007, 2008, 2009 Rob Buis <buis@kde.org>
 * Copyright (C) 2007 Eric Seidel <eric@webkit.org>
 * Copyright (C) 2009 Google, Inc.
 * Copyright (C) Research In Motion Limited 2011. All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "config.h"
#include "RenderSVGRoot.h"

#include "Frame.h"
#include "GraphicsContext.h"
#include "HitTestResult.h"
#include "LayoutRepainter.h"
#include "Page.h"
#include "RenderIterator.h"
#include "RenderLayer.h"
#include "RenderNamedFlowFragment.h"
#include "RenderSVGResource.h"
#include "RenderSVGResourceContainer.h"
#include "RenderSVGResourceFilter.h"
#include "RenderView.h"
#include "SVGImage.h"
#include "SVGRenderingContext.h"
#include "SVGResources.h"
#include "SVGResourcesCache.h"
#include "SVGSVGElement.h"
#include "SVGViewSpec.h"
#include "TransformState.h"
#include <wtf/StackStats.h>

namespace WebCore {

RenderSVGRoot::RenderSVGRoot(SVGSVGElement& element, RenderStyle&& style)
    : RenderReplaced(element, WTFMove(style))
    , m_objectBoundingBoxValid(false)
    , m_isLayoutSizeChanged(false)
    , m_needsBoundariesOrTransformUpdate(true)
    , m_hasSVGShadow(false)
    , m_hasBoxDecorations(false)
{
}

RenderSVGRoot::~RenderSVGRoot()
{
}

SVGSVGElement& RenderSVGRoot::svgSVGElement() const
{
    return downcast<SVGSVGElement>(nodeForNonAnonymous());
}

void RenderSVGRoot::computeIntrinsicRatioInformation(FloatSize& intrinsicSize, double& intrinsicRatio) const
{
    // Spec: http://www.w3.org/TR/SVG/coords.html#IntrinsicSizing
    // SVG needs to specify how to calculate some intrinsic sizing properties to enable inclusion within other languages.

    // The intrinsic aspect ratio of the viewport of SVG content is necessary for example, when including SVG from an ‘object’
    // element in HTML styled with CSS. It is possible (indeed, common) for an SVG graphic to have an intrinsic aspect ratio but
    // not to have an intrinsic width or height. The intrinsic aspect ratio must be calculated based upon the following rules:
    // - The aspect ratio is calculated by dividing a width by a height.
    // - If the ‘width’ and ‘height’ of the rootmost ‘svg’ element are both specified with unit identifiers (in, mm, cm, pt, pc,
    //   px, em, ex) or in user units, then the aspect ratio is calculated from the ‘width’ and ‘height’ attributes after
    //   resolving both values to user units.
    intrinsicSize.setWidth(floatValueForLength(svgSVGElement().intrinsicWidth(), 0));
    intrinsicSize.setHeight(floatValueForLength(svgSVGElement().intrinsicHeight(), 0));


    if (!intrinsicSize.isEmpty())
        intrinsicRatio = intrinsicSize.width() / static_cast<double>(intrinsicSize.height());
    else {
        // - If either/both of the ‘width’ and ‘height’ of the rootmost ‘svg’ element are in percentage units (or omitted), the
        //   aspect ratio is calculated from the width and height values of the ‘viewBox’ specified for the current SVG document
        //   fragment. If the ‘viewBox’ is not correctly specified, or set to 'none', the intrinsic aspect ratio cannot be
        //   calculated and is considered unspecified.
        FloatSize viewBoxSize = svgSVGElement().viewBox().size();
        if (!viewBoxSize.isEmpty()) {
            // The viewBox can only yield an intrinsic ratio, not an intrinsic size.
            intrinsicRatio = viewBoxSize.width() / static_cast<double>(viewBoxSize.height());
        }
    }
}

bool RenderSVGRoot::isEmbeddedThroughSVGImage() const
{
    return isInSVGImage(&svgSVGElement());
}

bool RenderSVGRoot::isEmbeddedThroughFrameContainingSVGDocument() const
{
    // If our frame has an owner renderer, we're embedded through eg. object/embed/iframe,
    // but we only negotiate if we're in an SVG document.
    if (!frame().ownerRenderer())
        return false;
    return frame().document()->isSVGDocument();
}

static inline LayoutUnit resolveLengthAttributeForSVG(const Length& length, float scale, float maxSize)
{
    return valueForLength(length, maxSize) * (length.isFixed() ? scale : 1);
}

LayoutUnit RenderSVGRoot::computeReplacedLogicalWidth(ShouldComputePreferred shouldComputePreferred) const
{
    // When we're embedded through SVGImage (border-image/background-image/<html:img>/...) we're forced to resize to a specific size.
    if (!m_containerSize.isEmpty())
        return m_containerSize.width();

    if (isEmbeddedThroughFrameContainingSVGDocument())
        return containingBlock()->availableLogicalWidth();

    // SVG embedded via SVGImage (background-image/border-image/etc) / Inline SVG.
    return RenderReplaced::computeReplacedLogicalWidth(shouldComputePreferred);
}

LayoutUnit RenderSVGRoot::computeReplacedLogicalHeight(std::optional<LayoutUnit> estimatedUsedWidth) const
{
    // When we're embedded through SVGImage (border-image/background-image/<html:img>/...) we're forced to resize to a specific size.
    if (!m_containerSize.isEmpty())
        return m_containerSize.height();

    if (isEmbeddedThroughFrameContainingSVGDocument())
        return containingBlock()->availableLogicalHeight(IncludeMarginBorderPadding);

    // SVG embedded via SVGImage (background-image/border-image/etc) / Inline SVG.
    return RenderReplaced::computeReplacedLogicalHeight(estimatedUsedWidth);
}

void RenderSVGRoot::layout()
{
    StackStats::LayoutCheckPoint layoutCheckPoint;
    ASSERT(needsLayout());

    m_resourcesNeedingToInvalidateClients.clear();

    // Arbitrary affine transforms are incompatible with LayoutState.
    LayoutStateDisabler layoutStateDisabler(view());

    bool needsLayout = selfNeedsLayout();
    LayoutRepainter repainter(*this, checkForRepaintDuringLayout() && needsLayout);

    LayoutSize oldSize = size();
    updateLogicalWidth();
    updateLogicalHeight();
    buildLocalToBorderBoxTransform();

    m_isLayoutSizeChanged = needsLayout || (svgSVGElement().hasRelativeLengths() && oldSize != size());
    SVGRenderSupport::layoutChildren(*this, needsLayout || SVGRenderSupport::filtersForceContainerLayout(*this));

    if (!m_resourcesNeedingToInvalidateClients.isEmpty()) {
        // Invalidate resource clients, which may mark some nodes for layout.
        for (auto& resource :  m_resourcesNeedingToInvalidateClients) {
            resource->removeAllClientsFromCache();
            SVGResourcesCache::clientStyleChanged(*resource, StyleDifferenceLayout, resource->style());
        }

        m_isLayoutSizeChanged = false;
        SVGRenderSupport::layoutChildren(*this, false);
    }

    // At this point LayoutRepainter already grabbed the old bounds,
    // recalculate them now so repaintAfterLayout() uses the new bounds.
    if (m_needsBoundariesOrTransformUpdate) {
        updateCachedBoundaries();
        m_needsBoundariesOrTransformUpdate = false;
    }

    clearOverflow();
    if (!shouldApplyViewportClip()) {
        FloatRect contentRepaintRect = repaintRectInLocalCoordinates();
        contentRepaintRect = m_localToBorderBoxTransform.mapRect(contentRepaintRect);
        addVisualOverflow(enclosingLayoutRect(contentRepaintRect));
    }

    updateLayerTransform();
    m_hasBoxDecorations = isDocumentElementRenderer() ? hasVisibleBoxDecorationStyle() : hasVisibleBoxDecorations();
    invalidateBackgroundObscurationStatus();

    repainter.repaintAfterLayout();

    clearNeedsLayout();
}

bool RenderSVGRoot::shouldApplyViewportClip() const
{
    // the outermost svg is clipped if auto, and svg document roots are always clipped
    // When the svg is stand-alone (isDocumentElement() == true) the viewport clipping should always
    // be applied, noting that the window scrollbars should be hidden if overflow=hidden.
    return style().overflowX() == OHIDDEN
        || style().overflowX() == OAUTO
        || style().overflowX() == OSCROLL
        || this->isDocumentElementRenderer();
}

void RenderSVGRoot::paintReplaced(PaintInfo& paintInfo, const LayoutPoint& paintOffset)
{
    // An empty viewport disables rendering.
    if (borderBoxRect().isEmpty())
        return;

    // Don't paint, if the context explicitly disabled it.
    if (paintInfo.context().paintingDisabled())
        return;

    // SVG outlines are painted during PaintPhaseForeground.
    if (paintInfo.phase == PaintPhaseOutline || paintInfo.phase == PaintPhaseSelfOutline)
        return;

    // An empty viewBox also disables rendering.
    // (http://www.w3.org/TR/SVG/coords.html#ViewBoxAttribute)
    if (svgSVGElement().hasEmptyViewBox())
        return;

    // Don't paint if we don't have kids, except if we have filters we should paint those.
    if (!firstChild()) {
        auto* resources = SVGResourcesCache::cachedResourcesForRenderer(*this);
        if (!resources || !resources->filter()) {
            if (paintInfo.phase == PaintPhaseForeground)
                page().addRelevantUnpaintedObject(this, visualOverflowRect());
            return;
        }
    }

    if (paintInfo.phase == PaintPhaseForeground)
        page().addRelevantRepaintedObject(this, visualOverflowRect());

    // Make a copy of the PaintInfo because applyTransform will modify the damage rect.
    PaintInfo childPaintInfo(paintInfo);
    childPaintInfo.context().save();

    // Apply initial viewport clip
    if (shouldApplyViewportClip())
        childPaintInfo.context().clip(snappedIntRect(overflowClipRect(paintOffset, currentRenderNamedFlowFragment())));

    // Convert from container offsets (html renderers) to a relative transform (svg renderers).
    // Transform from our paint container's coordinate system to our local coords.
    IntPoint adjustedPaintOffset = roundedIntPoint(paintOffset);
    childPaintInfo.applyTransform(AffineTransform::translation(adjustedPaintOffset.x(), adjustedPaintOffset.y()) * localToBorderBoxTransform());

    // SVGRenderingContext must be destroyed before we restore the childPaintInfo.context(), because a filter may have
    // changed the context and it is only reverted when the SVGRenderingContext destructor finishes applying the filter.
    {
        SVGRenderingContext renderingContext;
        bool continueRendering = true;
        if (childPaintInfo.phase == PaintPhaseForeground) {
            renderingContext.prepareToRenderSVGContent(*this, childPaintInfo);
            continueRendering = renderingContext.isRenderingPrepared();
        }

        if (continueRendering) {
            childPaintInfo.updateSubtreePaintRootForChildren(this);
            for (auto& child : childrenOfType<RenderElement>(*this))
                child.paint(childPaintInfo, location());
        }
    }

    childPaintInfo.context().restore();
}

void RenderSVGRoot::willBeDestroyed()
{
    RenderBlock::removePercentHeightDescendant(const_cast<RenderSVGRoot&>(*this));

    SVGResourcesCache::clientDestroyed(*this);
    RenderReplaced::willBeDestroyed();
}

void RenderSVGRoot::insertedIntoTree()
{
    RenderReplaced::insertedIntoTree();
    SVGResourcesCache::clientWasAddedToTree(*this);
}

void RenderSVGRoot::willBeRemovedFromTree()
{
    SVGResourcesCache::clientWillBeRemovedFromTree(*this);
    RenderReplaced::willBeRemovedFromTree();
}

void RenderSVGRoot::styleDidChange(StyleDifference diff, const RenderStyle* oldStyle)
{
    if (diff == StyleDifferenceLayout)
        setNeedsBoundariesUpdate();

    // Box decorations may have appeared/disappeared - recompute status.
    if (diff == StyleDifferenceRepaint)
        m_hasBoxDecorations = hasVisibleBoxDecorationStyle();

    RenderReplaced::styleDidChange(diff, oldStyle);
    SVGResourcesCache::clientStyleChanged(*this, diff, style());
}

void RenderSVGRoot::addChild(RenderObject* child, RenderObject* beforeChild)
{
    RenderReplaced::addChild(child, beforeChild);
    SVGResourcesCache::clientWasAddedToTree(*child);
}

void RenderSVGRoot::removeChild(RenderObject& child)
{
    SVGResourcesCache::clientWillBeRemovedFromTree(child);
    RenderReplaced::removeChild(child);
}

// RenderBox methods will expect coordinates w/o any transforms in coordinates
// relative to our borderBox origin.  This method gives us exactly that.
void RenderSVGRoot::buildLocalToBorderBoxTransform()
{
    float scale = style().effectiveZoom();
    FloatPoint translate = svgSVGElement().currentTranslateValue();
    LayoutSize borderAndPadding(borderLeft() + paddingLeft(), borderTop() + paddingTop());
    m_localToBorderBoxTransform = svgSVGElement().viewBoxToViewTransform(contentWidth() / scale, contentHeight() / scale);
    if (borderAndPadding.isZero() && scale == 1 && translate == FloatPoint::zero())
        return;
    m_localToBorderBoxTransform = AffineTransform(scale, 0, 0, scale, borderAndPadding.width() + translate.x(), borderAndPadding.height() + translate.y()) * m_localToBorderBoxTransform;
}

const AffineTransform& RenderSVGRoot::localToParentTransform() const
{
    // Slightly optimized version of m_localToParentTransform = AffineTransform::translation(x(), y()) * m_localToBorderBoxTransform;
    m_localToParentTransform = m_localToBorderBoxTransform;
    if (x())
        m_localToParentTransform.setE(m_localToParentTransform.e() + roundToInt(x()));
    if (y())
        m_localToParentTransform.setF(m_localToParentTransform.f() + roundToInt(y()));
    return m_localToParentTransform;
}

LayoutRect RenderSVGRoot::clippedOverflowRectForRepaint(const RenderLayerModelObject* repaintContainer) const
{
    if (style().visibility() != VISIBLE && !enclosingLayer()->hasVisibleContent())
        return LayoutRect();

    FloatRect contentRepaintRect = m_localToBorderBoxTransform.mapRect(repaintRectInLocalCoordinates());
    contentRepaintRect.intersect(snappedIntRect(borderBoxRect()));

    LayoutRect repaintRect = enclosingLayoutRect(contentRepaintRect);
    if (m_hasBoxDecorations || hasRenderOverflow())
        repaintRect.unite(unionRect(localSelectionRect(false), visualOverflowRect()));

    return RenderReplaced::computeRectForRepaint(enclosingIntRect(repaintRect), repaintContainer);
}

FloatRect RenderSVGRoot::computeFloatRectForRepaint(const FloatRect& repaintRect, const RenderLayerModelObject* repaintContainer, bool fixed) const
{
    // Apply our local transforms (except for x/y translation), then our shadow,
    // and then call RenderBox's method to handle all the normal CSS Box model bits
    FloatRect adjustedRect = m_localToBorderBoxTransform.mapRect(repaintRect);

    const SVGRenderStyle& svgStyle = style().svgStyle();
    if (const ShadowData* shadow = svgStyle.shadow())
        shadow->adjustRectForShadow(adjustedRect);

    // Apply initial viewport clip
    if (shouldApplyViewportClip())
        adjustedRect.intersect(snappedIntRect(borderBoxRect()));

    if (m_hasBoxDecorations || hasRenderOverflow()) {
        // The selectionRect can project outside of the overflowRect, so take their union
        // for repainting to avoid selection painting glitches.
        LayoutRect decoratedRepaintRect = unionRect(localSelectionRect(false), visualOverflowRect());
        adjustedRect.unite(decoratedRepaintRect);
    }
    return RenderReplaced::computeRectForRepaint(enclosingIntRect(adjustedRect), repaintContainer, {fixed, false});
}

// This method expects local CSS box coordinates.
// Callers with local SVG viewport coordinates should first apply the localToBorderBoxTransform
// to convert from SVG viewport coordinates to local CSS box coordinates.
void RenderSVGRoot::mapLocalToContainer(const RenderLayerModelObject* repaintContainer, TransformState& transformState, MapCoordinatesFlags mode, bool* wasFixed) const
{
    ASSERT(mode & ~IsFixed); // We should have no fixed content in the SVG rendering tree.
    ASSERT(mode & UseTransforms); // mapping a point through SVG w/o respecting trasnforms is useless.

    RenderReplaced::mapLocalToContainer(repaintContainer, transformState, mode | ApplyContainerFlip, wasFixed);
}

const RenderObject* RenderSVGRoot::pushMappingToContainer(const RenderLayerModelObject* ancestorToStopAt, RenderGeometryMap& geometryMap) const
{
    return RenderReplaced::pushMappingToContainer(ancestorToStopAt, geometryMap);
}

void RenderSVGRoot::updateCachedBoundaries()
{
    SVGRenderSupport::computeContainerBoundingBoxes(*this, m_objectBoundingBox, m_objectBoundingBoxValid, m_strokeBoundingBox, m_repaintBoundingBoxExcludingShadow);
    SVGRenderSupport::intersectRepaintRectWithResources(*this, m_repaintBoundingBoxExcludingShadow);
    m_repaintBoundingBoxExcludingShadow.inflate(horizontalBorderAndPaddingExtent());

    m_repaintBoundingBox = m_repaintBoundingBoxExcludingShadow;
    SVGRenderSupport::intersectRepaintRectWithShadows(*this, m_repaintBoundingBox);
}

bool RenderSVGRoot::nodeAtPoint(const HitTestRequest& request, HitTestResult& result, const HitTestLocation& locationInContainer, const LayoutPoint& accumulatedOffset, HitTestAction hitTestAction)
{
    LayoutPoint pointInParent = locationInContainer.point() - toLayoutSize(accumulatedOffset);
    LayoutPoint pointInBorderBox = pointInParent - toLayoutSize(location());

    // Only test SVG content if the point is in our content box.
    // FIXME: This should be an intersection when rect-based hit tests are supported by nodeAtFloatPoint.
    if (contentBoxRect().contains(pointInBorderBox)) {
        FloatPoint localPoint = localToParentTransform().inverse().value_or(AffineTransform()).mapPoint(FloatPoint(pointInParent));

        for (RenderObject* child = lastChild(); child; child = child->previousSibling()) {
            // FIXME: nodeAtFloatPoint() doesn't handle rect-based hit tests yet.
            if (child->nodeAtFloatPoint(request, result, localPoint, hitTestAction)) {
                updateHitTestResult(result, pointInBorderBox);
                if (result.addNodeToListBasedTestResult(child->node(), request, locationInContainer) == HitTestProgress::Stop)
                    return true;
            }
        }
    }

    // If we didn't early exit above, we've just hit the container <svg> element. Unlike SVG 1.1, 2nd Edition allows container elements to be hit.
    if ((hitTestAction == HitTestBlockBackground || hitTestAction == HitTestChildBlockBackground) && visibleToHitTesting()) {
        // Only return true here, if the last hit testing phase 'BlockBackground' is executed. If we'd return true in the 'Foreground' phase,
        // hit testing would stop immediately. For SVG only trees this doesn't matter. Though when we have a <foreignObject> subtree we need
        // to be able to detect hits on the background of a <div> element. If we'd return true here in the 'Foreground' phase, we are not able
        // to detect these hits anymore.
        LayoutRect boundsRect(accumulatedOffset + location(), size());
        if (locationInContainer.intersects(boundsRect)) {
            updateHitTestResult(result, pointInBorderBox);
            if (result.addNodeToListBasedTestResult(&svgSVGElement(), request, locationInContainer, boundsRect) == HitTestProgress::Stop)
                return true;
        }
    }

    return false;
}

bool RenderSVGRoot::hasRelativeDimensions() const
{
    return svgSVGElement().intrinsicHeight().isPercentOrCalculated() || svgSVGElement().intrinsicWidth().isPercentOrCalculated();
}

void RenderSVGRoot::addResourceForClientInvalidation(RenderSVGResourceContainer* resource)
{
    RenderSVGRoot* svgRoot = SVGRenderSupport::findTreeRootObject(*resource);
    if (!svgRoot)
        return;
    svgRoot->m_resourcesNeedingToInvalidateClients.add(resource);
}

}
