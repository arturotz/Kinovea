﻿using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Data;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Windows.Forms;
using Kinovea.Services;

namespace Kinovea.ScreenManager
{
    /// <summary>
    /// This controls holds the keyframe name, color, timecode and comment.
    /// It is used in the side panel.
    /// </summary>
    public partial class KeyframeCommentBox : UserControl
    {
        #region Events
        /// <summary>
        /// Asks the main timeline to move to the time of this keyframe.
        /// </summary>
        public event EventHandler<TimeEventArgs> Selected;
        public event EventHandler<EventArgs<Guid>> Updated;
        #endregion

        #region Properties
        public Keyframe Keyframe
        {
            get { return keyframe; }
        }
        public bool Editing
        {
            get { return editingName || editingComment; }
        }
        #endregion

        #region Members
        private bool editingName;
        private bool editingComment;
        private Keyframe keyframe;
        private bool manualUpdate;
        private bool isSelected;
        private Pen penBorder = Pens.Silver;
        #endregion

        public KeyframeCommentBox()
        {
            InitializeComponent();
            this.BackColor = Color.WhiteSmoke;
            btnColor.BackColor = this.BackColor;
            rtbComment.BackColor = this.BackColor;
            btnSidebar.BackColor = this.BackColor;
            btnColor.FlatAppearance.MouseDownBackColor = this.BackColor;
            btnColor.FlatAppearance.MouseOverBackColor = this.BackColor;

            this.Paint += KeyframeCommentBox_Paint;
            btnColor.Paint += BtnColor_Paint;
            rtbComment.MouseWheel += RtbComment_MouseWheel;
        }

        private void RtbComment_MouseWheel(object sender, MouseEventArgs e)
        {
            ((HandledMouseEventArgs)e).Handled = true;
            this.OnMouseWheel(e);
        }

        private void KeyframeCommentBox_Paint(object sender, PaintEventArgs e)
        {
            Rectangle rect = new Rectangle(0, 0, this.ClientRectangle.Width - 1, this.ClientRectangle.Height - 1);
            e.Graphics.DrawRectangle(penBorder, rect);
        }

        #region Public methods

        /// <summary>
        /// Set the keyframe this control is wrapping.
        /// </summary>
        public void SetKeyframe(Keyframe keyframe)
        {
            this.keyframe = keyframe;
            if (keyframe == null)
                return;

            manualUpdate = true;
            tbName.Text = keyframe.Name;
            AfterNameChange();
            lblTimecode.Text = keyframe.TimeCode;
            
            // The font size is stored in the rich text format string itself.
            // Get rid of all formatting.
            rtbComment.Rtf = keyframe.Comments;
            string text = rtbComment.Text;
            rtbComment.Text = text;

            manualUpdate = false;
        }

        /// <summary>
        /// Update the highlight status based on the current timestamp.
        /// </summary>
        /// <param name="timestamp"></param>
        public void UpdateHighlight(long timestamp)
        {
            if (keyframe == null)
                return;

            isSelected = keyframe.Timestamp == timestamp;
            btnSidebar.BackColor = isSelected ? keyframe.Color : this.BackColor;
            rtbComment.BackColor = isSelected ? Color.White : this.BackColor;
            pnlComment.BackColor = rtbComment.BackColor;
        }

        /// <summary>
        /// Update the timecode after a change in time calibration.
        /// </summary>
        public void UpdateTimecode()
        {
            if (keyframe == null)
                return;

            lblTimecode.Text = keyframe.TimeCode;
        }
        #endregion

        private void BtnColor_Paint(object sender, PaintEventArgs e)
        {
            if (keyframe == null)
                return;

            e.Graphics.SmoothingMode = SmoothingMode.AntiAlias;

            Rectangle rect = new Rectangle(1, 1, btnColor.Width - 2, btnColor.Height - 2);
            using (SolidBrush brush = new SolidBrush(keyframe.Color))
                e.Graphics.FillEllipse(brush, rect);
        }

        private void btnColor_Click(object sender, EventArgs e)
        {
            if (keyframe == null || manualUpdate)
                return;

            FormColorPicker picker = new FormColorPicker(keyframe.Color);
            FormsHelper.Locate(picker);
            if (picker.ShowDialog() == DialogResult.OK)
            {
                keyframe.Color = picker.PickedColor;
                RaiseUpdated();
                AfterColorChange();
            }
            picker.Dispose();
        }

        private void tbName_TextChanged(object sender, EventArgs e)
        {
            if (keyframe == null || manualUpdate)
                return;

            if (string.IsNullOrEmpty(tbName.Text.Trim()))
            {
                // We can't allow an empty string so fall back to the timecode.
                keyframe.Name = "";
                manualUpdate = true;
                tbName.Text = keyframe.Name;
                manualUpdate = false;
            }
            else
            {
                keyframe.Name = tbName.Text;
            }

            RaiseUpdated();
            AfterNameChange();
        }

        private void AfterNameChange()
        {
            Size size = TextRenderer.MeasureText(tbName.Text, tbName.Font);
            tbName.Width = size.Width;
            tbName.Height = size.Height;
        }

        private void AfterColorChange()
        {
            btnSidebar.BackColor = isSelected ? keyframe.Color : Color.White;
        }

        private void rtbComment_TextChanged(object sender, EventArgs e)
        {
            UpdateTextHeight();

            if (keyframe == null || manualUpdate)
                return;

            keyframe.Comments = rtbComment.Rtf;
            RaiseUpdated();
        }

        private void UpdateTextHeight()
        {
            // Manually update the textbox height and manually grow the containers.
            // Other approaches tried:
            // - Having the container controls on autosize. broke work during init.
            // - Listening to ContentsResized event. Doesn't work with wordwrap.
            // - Using GetPreferredSize. 
            //
            // The setup for this is to have wordwrap, no scrollbars (None), anchors top-left-right.
            // Then coming here we manually compute the height of the control, and make sure it fits
            // the content. Then grow the containers.

            // Grow richtextbox.
            const int padding = 6;
            int numLines = rtbComment.GetLineFromCharIndex(rtbComment.TextLength) + 1;
            int border = rtbComment.Height - rtbComment.ClientSize.Height;
            rtbComment.Height = rtbComment.Font.Height * numLines + padding + border;

            // Grow containers.
            pnlComment.Height = rtbComment.Top + rtbComment.Height + rtbComment.Margin.Bottom + padding;
            this.Height = pnlComment.Top + pnlComment.Height + pnlComment.Margin.Bottom + padding;
        }

        private void tbName_Leave(object sender, EventArgs e)
        {
            editingName = false;
        }

        private void tbName_Enter(object sender, EventArgs e)
        {
            editingName = true;
        }

        private void rtbComment_Enter(object sender, EventArgs e)
        {
            editingComment = true;
        }

        private void rtbComment_Leave(object sender, EventArgs e)
        {
            editingComment = false;
        }

        private void KeyframeCommentBox_Enter(object sender, EventArgs e)
        {
            RaiseSelected();
        }

        private void KeyframeCommentBox_Click(object sender, EventArgs e)
        {
            RaiseSelected();
        }

        private void RaiseUpdated()
        {
            Updated?.Invoke(this, new EventArgs<Guid>(keyframe.Id));
        }

        private void RaiseSelected()
        {
            Selected?.Invoke(this, new TimeEventArgs(keyframe.Timestamp));
        }
    }
}
