#region License
/*
Copyright � Joan Charmant 2008-2009.
jcharmant@gmail.com

This file is part of Kinovea.

Kinovea is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License version 2
as published by the Free Software Foundation.

Kinovea is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Kinovea. If not, see http://www.gnu.org/licenses/.
*/
#endregion

using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Reflection;
using System.Resources;
using System.Threading;
using System.Windows.Forms;
using System.Xml;
using System.Xml.Serialization;

using Kinovea.ScreenManager.Languages;
using Kinovea.Services;
using Kinovea.Video;

namespace Kinovea.ScreenManager
{
    /// <summary>
    /// A Chronometer tool with multiple time sections.
    /// 
    /// Note on overlap and unclosed sections:
    /// The way the menus are set up allows for overlapping sections and open-ended sections (no end point).
    /// We don't do any special treatment for these.
    /// The sections are always ordered based on their starting point.
    /// 
    /// The boundary frames are part of the sections.
    /// </summary>
    [XmlType ("ChronoMulti")]
    public class DrawingChronoMulti : AbstractDrawing, IDecorable, IKvaSerializable
    {
        #region Properties
        public override string ToolDisplayName
        {
            get {  return "Advanced stopwatch"; }
        }
        public override int ContentHash
        {
            get
            {
                int iHash = visibleTimestamp.GetHashCode();
                iHash ^= invisibleTimestamp.GetHashCode();
                iHash ^= sections.GetHashCode();

                iHash ^= styleHelper.ContentHash;
                iHash ^= showLabel.GetHashCode();

                return iHash;
            }
        }
        public DrawingStyle DrawingStyle
        {
            get { return style;}
        }
        public Color Color
        {
            get { return styleHelper.GetBackgroundColor(255); }
        }
        public override InfosFading  InfosFading
        {
            // Fading is not modifiable from outside for chrono.
            // The chrono visibility uses its own mechanism.
            get { return null; }
            set { }
        }
        public override DrawingCapabilities Caps
        {
            get { return DrawingCapabilities.ConfigureColorSize | DrawingCapabilities.CopyPaste; }
        }
        public override List<ToolStripItem> ContextMenu
        {
            get
            {
                // This drawing needs to know the current time to produce the right menus.
                throw new InvalidProgramException();
            }
        }
        public List<VideoSection> VideoSections
        {
            get { return sections; }
        }
        #endregion

        #region Members
        // Core

        private long visibleTimestamp;               	// chrono becomes visible.
        private long invisibleTimestamp;             	// chrono stops being visible.
        private List<VideoSection> sections = new List<VideoSection>(); // start and stop counting.
        private long currentTimestamp;              // timestamp for context-menu operations.
        private string timecode;
        private bool showLabel;
        // Decoration
        private StyleHelper styleHelper = new StyleHelper();
        private DrawingStyle style;
        private InfosFading infosFading;
        private static readonly int allowedFramesOver = 12;  // Number of frames the chrono stays visible after the 'Hiding' point.
        private RoundedRectangle mainBackground = new RoundedRectangle();
        private RoundedRectangle lblBackground = new RoundedRectangle();

        #region Menu
        private ToolStripMenuItem mnuVisibility = new ToolStripMenuItem();
        private ToolStripMenuItem mnuHideBefore = new ToolStripMenuItem();
        private ToolStripMenuItem mnuShowBefore = new ToolStripMenuItem();
        private ToolStripMenuItem mnuHideAfter = new ToolStripMenuItem();
        private ToolStripMenuItem mnuShowAfter = new ToolStripMenuItem();

        private ToolStripMenuItem mnuAction = new ToolStripMenuItem();
        private ToolStripMenuItem mnuStart = new ToolStripMenuItem();
        private ToolStripMenuItem mnuStop = new ToolStripMenuItem();
        private ToolStripMenuItem mnuSplit = new ToolStripMenuItem();
        private ToolStripMenuItem mnuRenameSection = new ToolStripMenuItem();
        private ToolStripMenuItem mnuMoveCurrentStart = new ToolStripMenuItem();
        private ToolStripMenuItem mnuMoveCurrentEnd = new ToolStripMenuItem();
        private ToolStripMenuItem mnuMovePreviousEnd = new ToolStripMenuItem();
        private ToolStripMenuItem mnuMoveNextStart = new ToolStripMenuItem();
        private ToolStripMenuItem mnuDeleteSection = new ToolStripMenuItem();

        private ToolStripMenuItem mnuShowLabel = new ToolStripMenuItem();
        #endregion

        private static readonly log4net.ILog log = log4net.LogManager.GetLogger(System.Reflection.MethodBase.GetCurrentMethod().DeclaringType);
        #endregion

        #region Constructors
        public DrawingChronoMulti(PointF p, long start, long averageTimeStampsPerFrame, DrawingStyle preset = null)
        {
            // Core
            visibleTimestamp = 0;
            invisibleTimestamp = long.MaxValue;
            mainBackground.Rectangle = new RectangleF(p, SizeF.Empty);

            timecode = "error";

            styleHelper.Bicolor = new Bicolor(Color.Black);
            styleHelper.Font = new Font("Arial", 16, FontStyle.Bold);
            styleHelper.Clock = false;
            if (preset == null)
                preset = ToolManager.GetStylePreset("ChronoMulti");

            style = preset.Clone();
            BindStyle();

            // We use the InfosFading utility to fade the chrono away.
            // The refererence frame will be the frame at which fading start.
            // Must be updated on "Hide" menu.
            infosFading = new InfosFading(invisibleTimestamp, averageTimeStampsPerFrame);
            infosFading.FadingFrames = allowedFramesOver;
            infosFading.UseDefault = false;

            InitializeMenus();
        }

        public DrawingChronoMulti(XmlReader xmlReader, PointF scale, TimestampMapper timestampMapper, Metadata metadata)
            : this(PointF.Empty, 0, 1, null)
        {
            ReadXml(xmlReader, scale, timestampMapper);
        }

        private void InitializeMenus()
        {
            // Visibility menus.
            mnuShowBefore.Image = Properties.Drawings.showbefore;
            mnuShowAfter.Image = Properties.Drawings.showafter;
            mnuHideBefore.Image = Properties.Drawings.hidebefore;
            mnuHideAfter.Image = Properties.Drawings.hideafter;
            mnuShowBefore.Click += MnuShowBefore_Click;
            mnuShowAfter.Click += MnuShowAfter_Click;
            mnuHideBefore.Click += MnuHideBefore_Click;
            mnuHideAfter.Click += MnuHideAfter_Click;
            mnuVisibility.Image = Properties.Drawings.persistence;
            mnuVisibility.DropDownItems.AddRange(new ToolStripItem[] { 
                mnuShowBefore, 
                mnuShowAfter, 
                new ToolStripSeparator(), 
                mnuHideBefore, 
                mnuHideAfter });

            // Action menus
            mnuAction.Image = Properties.Drawings.stopwatch;
            mnuStart.Image = Properties.Drawings.chronostart;
            mnuStop.Image = Properties.Drawings.chronostop;
            mnuSplit.Image = Properties.Drawings.chrono_split;
            mnuRenameSection.Image = Properties.Resources.rename;
            mnuMoveCurrentStart.Image = Properties.Resources.chronosectionstart;
            mnuMoveCurrentEnd.Image = Properties.Resources.chronosectionend;
            mnuMovePreviousEnd.Image = Properties.Resources.chronosectionend;
            mnuMoveNextStart.Image = Properties.Resources.chronosectionstart;
            mnuDeleteSection.Image = Properties.Resources.bin_empty;

            mnuStart.Click += mnuStart_Click;
            mnuStop.Click += mnuStop_Click;
            mnuSplit.Click += mnuSplit_Click;
            //mnuRenameSection.Click += mnuRenameSection_Click;
            mnuMoveCurrentStart.Click += mnuMoveCurrentStart_Click;
            mnuMoveCurrentEnd.Click += mnuMoveCurrentEnd_Click;
            mnuMovePreviousEnd.Click += mnuMovePreviousEnd_Click;
            mnuMoveNextStart.Click += mnuMoveNextStart_Click;
            mnuDeleteSection.Click += mnuDeleteSection_Click;


            mnuShowLabel.Click += mnuShowLabel_Click;
        }
        #endregion

        #region AbstractDrawing Implementation
        public override void Draw(Graphics canvas, DistortionHelper distorter, IImageToViewportTransformer transformer, bool selected, long currentTimestamp)
        {
            if (currentTimestamp < visibleTimestamp)
                return;

            double opacityFactor = 1.0;
            if (currentTimestamp > invisibleTimestamp)
                opacityFactor = infosFading.GetOpacityFactor(currentTimestamp);

            if (opacityFactor <= 0)
                return;

            timecode = GetTimecode(currentTimestamp);
            string text = timecode;

            using (SolidBrush brushBack = styleHelper.GetBackgroundBrush((int)(opacityFactor * 128)))
            using (SolidBrush brushText = styleHelper.GetForegroundBrush((int)(opacityFactor * 255)))
            using (Font fontText = styleHelper.GetFont((float)transformer.Scale))
            {
                SizeF textSize = canvas.MeasureString(text, fontText);
                Point bgLocation = transformer.Transform(mainBackground.Rectangle.Location);
                Size bgSize = new Size((int)textSize.Width, (int)textSize.Height);

                SizeF untransformed = transformer.Untransform(textSize);
                mainBackground.Rectangle = new RectangleF(mainBackground.Rectangle.Location, untransformed);

                Rectangle rect = new Rectangle(bgLocation, bgSize);
                int roundingRadius = fontText.Height / 4;
                RoundedRectangle.Draw(canvas, rect, brushBack, roundingRadius, false, false, null);
                canvas.DrawString(text, fontText, brushText, rect.Location);

                if (showLabel && name.Length > 0)
                {
                    using (Font fontLabel = styleHelper.GetFont((float)transformer.Scale * 0.5f))
                    {
                        // Note: the alignment here assumes fixed margins of the rounded rectangle class.
                        SizeF lblTextSize = canvas.MeasureString(name, fontLabel);
                        int labelRoundingRadius = fontLabel.Height / 3;
                        int top = rect.Location.Y - (int)lblTextSize.Height - roundingRadius - labelRoundingRadius;
                        Rectangle lblRect = new Rectangle(rect.Location.X, top, (int)lblTextSize.Width, (int)lblTextSize.Height);
                        RoundedRectangle.Draw(canvas, lblRect, brushBack, labelRoundingRadius, true, false, null);
                        canvas.DrawString(name, fontLabel, brushText, lblRect.Location);
                    }
                }
            }
        }
        public override int HitTest(PointF point, long currentTimestamp, DistortionHelper distorter, IImageToViewportTransformer transformer, bool zooming)
        {
            // Convention: miss = -1, object = 0, handle = n.
            int result = -1;
            long maxHitTimeStamps = invisibleTimestamp;
            if (maxHitTimeStamps != long.MaxValue)
                maxHitTimeStamps += (allowedFramesOver * parentMetadata.AverageTimeStampsPerFrame);

            if (currentTimestamp >= visibleTimestamp && currentTimestamp <= maxHitTimeStamps)
            {
                using (Font fontText = styleHelper.GetFont(1.0f))
                {
                    int roundingRadius = fontText.Height / 4;
                    result = mainBackground.HitTest(point, true, (int)(roundingRadius * 1.8f), transformer);
                }

                if(result < 0)
                    result = lblBackground.HitTest(point, false, 0, transformer);
            }

            return result;
        }
        public override void MoveHandle(PointF point, int handleNumber, Keys modifiers)
        {
            // Invisible handler to change font size.
            int targetHeight = (int)(point.Y - mainBackground.Rectangle.Location.Y);
            StyleElementFontSize elem = style.Elements["font size"] as StyleElementFontSize;
            elem.ForceSize(targetHeight, timecode, styleHelper.Font);
            UpdateLabelRectangle();
        }
        public override void MoveDrawing(float dx, float dy, Keys modifierKeys, bool zooming)
        {
            mainBackground.Move(dx, dy);
            lblBackground.Move(dx, dy);
        }
        public override PointF GetCopyPoint()
        {
            return mainBackground.Rectangle.Center();
        }
        #endregion

        #region KVA Serialization
        public void WriteXml(XmlWriter w, SerializationFilter filter)
        {
            if (ShouldSerializeCore(filter))
            {
                w.WriteElementString("Position", XmlHelper.WritePointF(mainBackground.Rectangle.Location));

                w.WriteStartElement("Values");

                w.WriteElementString("Visible", (visibleTimestamp == long.MaxValue) ? "-1" : visibleTimestamp.ToString());
                w.WriteElementString("Invisible", (invisibleTimestamp == long.MaxValue) ? "-1" : invisibleTimestamp.ToString());
                
                if (sections.Count > 0)
                {
                    w.WriteStartElement("Sections");

                    foreach (VideoSection section in sections)
                        w.WriteElementString("Section", XmlHelper.WriteVideoSection(section));

                    w.WriteEndElement();
                }

                // </values>
                w.WriteEndElement();
            }

            if (ShouldSerializeStyle(filter))
            {
                // Label
                w.WriteStartElement("Label");
                w.WriteElementString("Show", showLabel.ToString().ToLower());
                w.WriteEndElement();

                w.WriteStartElement("DrawingStyle");
                style.WriteXml(w);
                w.WriteEndElement();
            }
        }

        public MeasuredDataTime CollectMeasuredData()
        {
            MeasuredDataTime mdt = new MeasuredDataTime();
            mdt.Name = name;

            //if (!styleHelper.Clock && startCountingTimestamp != long.MaxValue && stopCountingTimestamp != long.MaxValue)
            //{
            //    float userStart = parentMetadata.GetNumericalTime(startCountingTimestamp, TimeType.UserOrigin);
            //    float userStop = parentMetadata.GetNumericalTime(stopCountingTimestamp, TimeType.UserOrigin);
            //    float userDuration = parentMetadata.GetNumericalTime(stopCountingTimestamp - startCountingTimestamp, TimeType.Absolute);

            //    mdt.Start = userStart;
            //    mdt.Stop = userStop;
            //    mdt.Duration = userDuration;
            //}

            return mdt;
        }


        public void ReadXml(XmlReader xmlReader, PointF scale, TimestampMapper timestampMapper)
        {
            if (xmlReader.MoveToAttribute("id"))
                identifier = new Guid(xmlReader.ReadContentAsString());

            if (xmlReader.MoveToAttribute("name"))
                name = xmlReader.ReadContentAsString();

            xmlReader.ReadStartElement();

            while(xmlReader.NodeType == XmlNodeType.Element)
            {
                switch(xmlReader.Name)
                {
                    case "Position":
                        PointF p = XmlHelper.ParsePointF(xmlReader.ReadElementContentAsString());
                        mainBackground.Rectangle = new RectangleF(p.Scale(scale.X, scale.Y), SizeF.Empty);
                        break;
                    case "Values":
                        ParseWorkingValues(xmlReader, timestampMapper);
                        break;
                    case "DrawingStyle":
                        style = new DrawingStyle(xmlReader);
                        BindStyle();
                        break;
                    case "Label":
                        ParseLabel(xmlReader);
                        break;
                    default:
                        string unparsed = xmlReader.ReadOuterXml();
                        log.DebugFormat("Unparsed content in KVA XML: {0}", unparsed);
                        break;
                }
            }

            xmlReader.ReadEndElement();
            SanityCheckValues();
        }
        private void ParseWorkingValues(XmlReader xmlReader, TimestampMapper timestampMapper)
        {
            if(timestampMapper == null)
            {
                xmlReader.ReadOuterXml();
                return;
            }

            xmlReader.ReadStartElement();

            while(xmlReader.NodeType == XmlNodeType.Element)
            {
                switch(xmlReader.Name)
                {
                    case "Visible":
                        visibleTimestamp = timestampMapper(xmlReader.ReadElementContentAsLong());
                        break;
                    case "Invisible":
                        long hide = xmlReader.ReadElementContentAsLong();
                        invisibleTimestamp = (hide == -1) ? long.MaxValue : timestampMapper(hide);
                        break;
                    case "Sections":
                        ParseSections(xmlReader, timestampMapper);
                        break;
                    default:
                        string unparsed = xmlReader.ReadOuterXml();
                        log.DebugFormat("Unparsed content in KVA XML: {0}", unparsed);
                        break;
                }
            }

            xmlReader.ReadEndElement();
        }

        private void ParseSections(XmlReader xmlReader, TimestampMapper timestampMapper)
        {
            sections.Clear();

            if (timestampMapper == null)
            {
                xmlReader.ReadOuterXml();
                return;
            }

            xmlReader.ReadStartElement();

            while (xmlReader.NodeType == XmlNodeType.Element)
            {
                switch (xmlReader.Name)
                {
                    case "Section":
                        VideoSection section = XmlHelper.ParseVideoSection(xmlReader.ReadElementContentAsString());
                        section = new VideoSection(timestampMapper(section.Start), timestampMapper(section.End));
                        InsertSection(section);

                        break;
                    default:
                        string unparsed = xmlReader.ReadOuterXml();
                        log.DebugFormat("Unparsed content in KVA XML: {0}", unparsed);
                        break;
                }
            }

            xmlReader.ReadEndElement();
        }
        private void SanityCheckValues()
        {
            visibleTimestamp = Math.Max(visibleTimestamp, 0);
            invisibleTimestamp = Math.Max(invisibleTimestamp, 0);
        }
        private void ParseLabel(XmlReader xmlReader)
        {
            xmlReader.ReadStartElement();

            while(xmlReader.NodeType == XmlNodeType.Element)
            {
                switch(xmlReader.Name)
                {
                    case "Show":
                        showLabel = XmlHelper.ParseBoolean(xmlReader.ReadElementContentAsString());
                        break;
                    default:
                        string unparsed = xmlReader.ReadOuterXml();
                        log.DebugFormat("Unparsed content in KVA XML: {0}", unparsed);
                        break;
                }
            }

            xmlReader.ReadEndElement();
        }
        #endregion

        #region Tool-specific context menu


        /// <summary>
        /// Get the context menu according to the current time and locale.
        /// </summary>
        public List<ToolStripItem> GetContextMenu(long timestamp)
        {
            List<ToolStripItem> contextMenu = new List<ToolStripItem>();
            ReloadMenusCulture();

            // Backup the time globally for use in the event handlers callbacks.
            currentTimestamp = timestamp;
            
            // The context menu depends on whether we are on a live or dead section.
            mnuAction.DropDownItems.Clear();
            int sectionIndex = GetSectionIndex(currentTimestamp);
            
            if (sectionIndex >= 0)
            {
                // Live section.
                mnuAction.DropDownItems.AddRange(new ToolStripItem[] {
                    mnuStop,
                    mnuSplit,
                    new ToolStripSeparator(),
                    mnuRenameSection,
                    mnuMoveCurrentStart,
                    mnuMoveCurrentEnd,
                    mnuDeleteSection
                });
            }
            else
            {
                // Dead section.
                mnuAction.DropDownItems.AddRange(new ToolStripItem[] {
                    mnuStart,
                    new ToolStripSeparator(),
                    mnuMovePreviousEnd,
                    mnuMoveNextStart,
                });
            }

            mnuMovePreviousEnd.Enabled = !IsBeforeFirstSection(sectionIndex);
            mnuMoveNextStart.Enabled = !IsAfterLastSection(sectionIndex);
            mnuShowLabel.Checked = showLabel;
            
            contextMenu.AddRange(new ToolStripItem[] { 
                mnuVisibility, 
                mnuAction,
                mnuShowLabel
            });

            return contextMenu;
        }

        private void ReloadMenusCulture()
        {
            // Visibility
            mnuVisibility.Text = ScreenManagerLang.Generic_Visibility;
            mnuHideBefore.Text = ScreenManagerLang.mnuHideBefore;
            mnuShowBefore.Text = ScreenManagerLang.mnuShowBefore;
            mnuHideAfter.Text = ScreenManagerLang.mnuHideAfter;
            mnuShowAfter.Text = ScreenManagerLang.mnuShowAfter;

            // Action
            mnuAction.Text = "Action";

            // When we are on a live section.
            mnuStop.Text = "Stop: end the current time section on this frame";
            mnuSplit.Text = "Split: end the current time section on this frame and start a new one";
            mnuRenameSection.Text = "Rename the current time section";
            mnuMoveCurrentStart.Text = "Move the start of the current time section to this frame";
            mnuMoveCurrentEnd.Text = "Move the end of the current time section to this frame";
            mnuDeleteSection.Text = "Delete the current time section";

            // When we are on a dead section.
            mnuStart.Text = "Start a new time section on this frame";
            mnuMovePreviousEnd.Text = "Move the end of the previous section to this frame";
            mnuMoveNextStart.Text = "Move the start of the next section to this frame";

            // Display.
            mnuShowLabel.Text = ScreenManagerLang.mnuShowLabel;
        }

        #region Visibility
        private void MnuShowBefore_Click(object sender, EventArgs e)
        {
            CaptureMemento(SerializationFilter.Core);
            visibleTimestamp = 0;
            InvalidateFromMenu(sender);
        }

        private void MnuShowAfter_Click(object sender, EventArgs e)
        {
            CaptureMemento(SerializationFilter.Core);
            invisibleTimestamp = long.MaxValue;
            infosFading.ReferenceTimestamp = invisibleTimestamp;
            InvalidateFromMenu(sender);
        }

        private void MnuHideBefore_Click(object sender, EventArgs e)
        {
            CaptureMemento(SerializationFilter.Core);
            visibleTimestamp = CurrentTimestampFromMenu(sender);
            InvalidateFromMenu(sender);
        }

        private void MnuHideAfter_Click(object sender, EventArgs e)
        {
            CaptureMemento(SerializationFilter.Core);
            invisibleTimestamp = CurrentTimestampFromMenu(sender);
            infosFading.ReferenceTimestamp = invisibleTimestamp;
            InvalidateFromMenu(sender);
        }
        #endregion

        private void mnuStart_Click(object sender, EventArgs e)
        {
            // Start a new section here.
            CaptureMemento(SerializationFilter.Core);
            
            InsertSection(new VideoSection(currentTimestamp, long.MaxValue));
            
            InvalidateFromMenu(sender);
            UpdateFramesMarkersFromMenu(sender);
        }

        private void mnuStop_Click(object sender, EventArgs e)
        {
            // Stop the current section here.
            int sectionIndex = GetSectionIndex(currentTimestamp);
            if (sectionIndex < 0)
                return;

            CaptureMemento(SerializationFilter.Core);

            StopSection(sectionIndex, currentTimestamp);

            if (currentTimestamp > invisibleTimestamp)
                invisibleTimestamp = currentTimestamp;

            InvalidateFromMenu(sender);
            UpdateFramesMarkersFromMenu(sender);
        }

        private void mnuSplit_Click(object sender, EventArgs e)
        {
            // Stop the current section here and start a new one.
            int sectionIndex = GetSectionIndex(currentTimestamp);
            if (sectionIndex < 0)
                return;

            CaptureMemento(SerializationFilter.Core);
            
            StopSection(sectionIndex, currentTimestamp);
            InsertSection(new VideoSection(currentTimestamp, long.MaxValue));

            if (currentTimestamp > invisibleTimestamp)
                invisibleTimestamp = currentTimestamp;

            InvalidateFromMenu(sender);
            UpdateFramesMarkersFromMenu(sender);
        }

        private void mnuMoveCurrentStart_Click(object sender, EventArgs e)
        {
            int sectionIndex = GetSectionIndex(currentTimestamp);
            if (sectionIndex < 0)
                return;

            CaptureMemento(SerializationFilter.Core);

            sections[sectionIndex] = new VideoSection(currentTimestamp, sections[sectionIndex].End);

            InvalidateFromMenu(sender);
            UpdateFramesMarkersFromMenu(sender);
        }

        private void mnuMoveCurrentEnd_Click(object sender, EventArgs e)
        {
            // Technically "Move current end" is the same as "Stop", but we keep it for symmetry purposes.
            int sectionIndex = GetSectionIndex(currentTimestamp);
            if (sectionIndex < 0)
                return;

            CaptureMemento(SerializationFilter.Core);

            sections[sectionIndex] = new VideoSection(sections[sectionIndex].Start, currentTimestamp);

            InvalidateFromMenu(sender);
            UpdateFramesMarkersFromMenu(sender);
        }

        private void mnuMovePreviousEnd_Click(object sender, EventArgs e)
        {
            int sectionIndex = GetSectionIndex(currentTimestamp);
            if (sectionIndex >= 0 || IsBeforeFirstSection(sectionIndex))
                return;

            CaptureMemento(SerializationFilter.Core);

            int prevIndex = -(sectionIndex + 2);
            sections[prevIndex] = new VideoSection(sections[prevIndex].Start, currentTimestamp);

            InvalidateFromMenu(sender);
            UpdateFramesMarkersFromMenu(sender);
        }

        private void mnuMoveNextStart_Click(object sender, EventArgs e)
        {
            int sectionIndex = GetSectionIndex(currentTimestamp);
            if (sectionIndex >= 0 || IsAfterLastSection(sectionIndex))
                return;

            CaptureMemento(SerializationFilter.Core);

            int nextIndex = -(sectionIndex + 1);
            sections[nextIndex] = new VideoSection(currentTimestamp, sections[nextIndex].End);

            InvalidateFromMenu(sender);
            UpdateFramesMarkersFromMenu(sender);
        }

    private void mnuDeleteSection_Click(object sender, EventArgs e)
        {
            int sectionIndex = GetSectionIndex(currentTimestamp);
            if (sectionIndex < 0)
                return;

            CaptureMemento(SerializationFilter.Core);
            
            sections.RemoveAt(sectionIndex);

            InvalidateFromMenu(sender);
            UpdateFramesMarkersFromMenu(sender);
        }

        private void mnuShowLabel_Click(object sender, EventArgs e)
        {
            CaptureMemento(SerializationFilter.Style);
            showLabel = !mnuShowLabel.Checked;
            InvalidateFromMenu(sender);
        }
        #endregion

        #region Lower level helpers
        private void BindStyle()
        {
            DrawingStyle.SanityCheck(style, ToolManager.GetStylePreset("ChronoMulti"));
            style.Bind(styleHelper, "Bicolor", "color");
            style.Bind(styleHelper, "Font", "font size");
        }
        private void UpdateLabelRectangle()
        {
            using(Font f = styleHelper.GetFont(0.5F))
            {
                SizeF size = TextHelper.MeasureString(name, f);
                lblBackground.Rectangle = new RectangleF(
                    mainBackground.X, mainBackground.Y - lblBackground.Rectangle.Height, size.Width + 11, size.Height);
            }
        }

        /// <summary>
        /// Insert a new section into the list.
        /// </summary>
        private void InsertSection(VideoSection section)
        {
            // Find insertion point and insert the new section there.
            bool found = false;
            int i = 0;
            for (i = 0; i < sections.Count; i++)
            {
                if (sections[i].Start < section.Start)
                    continue;

                found = true;
                break;
            }

            if (!found)
                sections.Add(section);
            else
                sections.Insert(i, section);

        }

        /// <summary>
        /// Update the end time of a specific section.
        /// </summary>
        private void StopSection(int index, long timestamp)
        {
            sections[index] = new VideoSection(sections[index].Start, timestamp);
        }

        /// <summary>
        /// Returns the section index that timestamp is in. 
        /// Otherwise returns a negative number based on the next section:
        /// -1 if we are before the first live zone, 
        /// -2 if we are after the first and before the second, 
        /// -n if we are before the n-th section.
        /// -(n+1) if we are after the last section.
        /// 
        /// In case of overlapping sections, returns the section with the earliest starting point.
        /// An open-ended section contains all the timestamps after its start.
        /// </summary>
        private int GetSectionIndex(long timestamp)
        {
            int result = -1;
            for (int i = 0; i < sections.Count; i++)
            {
                // Before the start of this section. 
                if (timestamp < sections[i].Start)
                    break;

                // Between start and end of this section.
                // The end of the section is part of the section.
                if (timestamp <= sections[i].End)
                {
                    result = i;
                    break;
                }

                // After that section.
                result--;
            }

            return result;
        }

        /// <summary>
        /// Returns true if this dead-zone index is before the first section.
        /// </summary>
        private bool IsBeforeFirstSection(int index)
        {
            return index == -1;
        }

        /// <summary>
        /// Returns true if this dead-zone index is after the last section.
        /// </summary>
        private bool IsAfterLastSection(int index)
        {
            return index == -(sections.Count + 1);
        }

        private string GetTimecode(long currentTimestamp)
        {
            // TODO:
            // This will be replaced by a full table of values.


            // Stopwatch mode.
            long durationTimestamps = 0;
            int sectionIndex = GetSectionIndex(currentTimestamp);
            if (sectionIndex >= 0)
            {
                durationTimestamps = currentTimestamp - sections[sectionIndex].Start;
            }
            else
            {
                durationTimestamps = 0;
            }

            return parentMetadata.TimeCodeBuilder(durationTimestamps, TimeType.Absolute, TimecodeFormat.Unknown, true);
        }

        /// <summary>
        /// Capture the current state to the undo/redo stack.
        /// </summary>
        private void CaptureMemento(SerializationFilter filter)
        {
            var memento = new HistoryMementoModifyDrawing(parentMetadata, parentMetadata.ChronoManager.Id, this.Id, this.Name, filter);
            parentMetadata.HistoryStack.PushNewCommand(memento);
        }
        #endregion
    }
}