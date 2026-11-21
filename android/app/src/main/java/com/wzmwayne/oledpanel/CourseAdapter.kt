package com.wzmwayne.oledpanel

import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.TextView
import androidx.recyclerview.widget.RecyclerView

class CourseAdapter(
    private val onLongClick: (Course) -> Unit
) : RecyclerView.Adapter<CourseAdapter.VH>() {

    private val items = ArrayList<Course>()

    fun submit(newItems: List<Course>) {
        items.clear()
        items.addAll(newItems)
        notifyDataSetChanged()
    }

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): VH {
        val view = LayoutInflater.from(parent.context)
            .inflate(R.layout.item_course, parent, false)
        return VH(view)
    }

    override fun getItemCount(): Int = items.size

    override fun onBindViewHolder(holder: VH, position: Int) {
        holder.bind(items[position])
    }

    inner class VH(itemView: View) : RecyclerView.ViewHolder(itemView) {
        private val tvName: TextView = itemView.findViewById(R.id.tvCourseName)
        private val tvMeta: TextView = itemView.findViewById(R.id.tvCourseMeta)
        private val tvParity: TextView = itemView.findViewById(R.id.tvCourseParity)

        fun bind(course: Course) {
            tvName.text = course.name.ifBlank { "未命名课程" }
            tvMeta.text = course.metaText()
            tvParity.text = course.parityText()
            itemView.setOnLongClickListener {
                onLongClick(course)
                true
            }
        }
    }
}
